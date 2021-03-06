#include "evaluator.hpp"
#include "action.hpp"
#include "expr.hpp"
#include "decode.hpp"
#include "type.hpp"
#include "decl.hpp"
#include "dumper.hpp"
#include "context.hpp"

#include <climits>
#include <random>
#include <sstream>
// testing only
#include <iostream>

namespace pip
{
  evaluator::evaluator(context& cxt, decl* prog, cap::packet& pkt, std::uint32_t physical_ports)
    : cxt(cxt), 
      prog(prog), 
      data(pkt), 
      arrival(pkt.timestamp()),
      ingress_port(), 
      physical_port(), 
      metadata(), 
      keyreg(), 
      decode(),
      dec(cxt)
  {    
    assert(cap::ethernet_ethertype(pkt.data()) == 0x800  &&
	   "Non-ethernet frames are not supported.\n");
    assert(cap::ipv4_protocol(pkt.data()) == 0x06 &&
	   "Non-TCP packets are not supported.\n");
    
    modified_buffer = new unsigned char[pkt.size()];
    std::memcpy(modified_buffer, pkt.data(), pkt.size());
    
    ingress_port = cap::tcp_src_port(pkt.data());

    std::random_device rand_device;
    std::mt19937 rand_engine(rand_device());
    std::uniform_int_distribution<std::uint32_t> rand_distribution(1, physical_ports);
    physical_port = rand_distribution(rand_engine);
    std::cout << "Packet received on port: " << physical_port << '\n';
    
    // Perform static initialization. We need to evaluate all of
    // the table definitions to load them with their static rules.
    auto program = static_cast<program_decl*>(prog);
    std::vector<decl*> tables;
    for(auto declaration : program->decls)
      if(auto table = dynamic_cast<table_decl*>(declaration)) {
	for(auto r : table->rules)
	  if(auto int_key = dynamic_cast<int_expr*>(r->key))
	    table->key_table.insert(int_key->val);
	tables.push_back(table);
      }

    // Load the instructions from the first table.
    current_table = static_cast<table_decl*>(tables.front());
    for(auto a : current_table->prep)
      eval.push_back(a);
  }

  evaluator::~evaluator()
  {
    if(modified_buffer)
      delete[] modified_buffer;
  }

  const action*
  evaluator::fetch()
  {
    const action* a = eval.front();
    if(a)
      std::cout << "there is an action in eval\n";
    eval.pop_front();
    return a;
  }

  void
  evaluator::step()
  {
    // If the action queue is empty, proceed to egress processing. This
    // will execute the actions in the action list. If the action list
    // is empty, then the program is complete.
    if (eval.empty()) {
      if (actions.empty())
        return;

      // This marks the beginning of egress processing.
      eval.insert(eval.end(), actions.begin(), actions.end());
      actions.clear();
    }
    
    const action* a = fetch();
    switch (get_kind(a)) {
      case ak_advance:
        return eval_advance(cast<advance_action>(a));
      case ak_copy:
        return eval_copy(cast<copy_action>(a));
      case ak_set:
        return eval_set(cast<set_action>(a));
      case ak_write:
        return eval_write(cast<write_action>(a));
      case ak_clear:
        return eval_clear(cast<clear_action>(a));
      case ak_drop:
        return eval_drop(cast<drop_action>(a));
      case ak_match:
        return eval_match(cast<match_action>(a));
      case ak_goto:
        return eval_goto(cast<goto_action>(a));
      case ak_output:
        return eval_output(cast<output_action>(a));
        
      default:
        break;   
    }

    throw std::logic_error("invalid action");
  }

  void
  evaluator::run()
  {
    while (!done())
      step();
  }

  void
  evaluator::eval_advance(const advance_action* a)
  {
    const auto n = static_cast<int_expr*>(a->amount);
    int amount = n->val;

    decode += amount;

    std::cout << "Decode: " << decode << '\n';
  }  

  // Copy n bits from position pos in a byte array into a 64-bit integer.
  static std::uint64_t
  data_to_key_reg(std::uint8_t* bytes, std::size_t pos, std::size_t n)
  {
    const std::uint8_t* current_byte = bytes + pos / CHAR_BIT;
    std::uint64_t reg = 0;

    std::size_t offset = pos % CHAR_BIT;

    // If the copy begins between two whole bytes,
    if(offset > 0) {
      // copy the specified bits.
      reg = *(current_byte++) & (0xff >> offset);

      // If there is nothing else to copy, shift off any excess and return.
      if(n <= CHAR_BIT - offset)
	return reg >> (CHAR_BIT - offset - n);
      // Remove the copied bits from n.
      else
	n -= CHAR_BIT - offset;
    }

    // Copy all remaining whole bytes.
    while(n >= CHAR_BIT) {
      reg = (reg << CHAR_BIT) + *(current_byte++);
      n -= CHAR_BIT;
    }

    // If some fraction of a byte remains to be copied, copy it.
    if(n > 0)
      reg = (reg << n) + (*current_byte >> (CHAR_BIT - n));
	
    return reg;
  }

  // Copy n bits from one byte array to another, starting at position pos.
  static void
  bitwise_copy(std::uint8_t* dst, std::uint8_t const * src,
	       std::size_t pos, std::size_t n)
  {
    std::uint8_t* current_byte = dst + (pos / CHAR_BIT);
    std::size_t i = 0;

    // if the copy begins in between two whole bytes
    if(pos % CHAR_BIT > 0) {
      // copy just what is needed
      *current_byte |= src[i] & (0xFF >> (pos % CHAR_BIT));    

      // if there is nothing else to copy, terminate
      if(n <= CHAR_BIT - pos % CHAR_BIT)
	return;
      // otherwise, even out n to start on the next whole byte
      // (n is now a multiple of CHAR_BIT)
      else
	n -= CHAR_BIT - pos % CHAR_BIT;

      ++current_byte;
      // note that we have already accessed the first element of src
      ++i;
    }

    // copy all whole bytes that remain
    for(; n >= CHAR_BIT; n -= CHAR_BIT, ++i)
      *current_byte++ = src[i];

    // if there is a remainder of less than CHAR_BIT bits,
    // copy just that portion of the byte.
    if(n > 0) {
      *current_byte |= src[i] & (0xff << (CHAR_BIT - n));
    }  
  }

  // Copy n bits from src register to dst register starting at position pos.
  static std::uint64_t
  reg_to_reg(std::uint64_t src, std::uint64_t dst, std::size_t pos, std::size_t n)
  {
    // Create a string of 1s starting at pos to the least significant bit
    std::uint64_t mask = (((~0 & (1U << pos)) - 1));
    // Make the string of 1s stop n bits after pos.
    mask &= ~((~0 & (((1U << (pos - n - 1))) - 1) << 1) + 1);

    dst &= mask;
    src &= ~mask;
    return dst | src;
  }

  // Copy n bits from a uint64_t into a byte array at position pos.
  // See data_to_key_reg and bitwise_copy for further documentation.
  static void
  reg_to_buf(std::uint8_t* bytes, std::uint64_t in,
	     std::size_t pos, std::size_t n)
  {
    std::uint8_t* current_byte = bytes + pos / CHAR_BIT;
    std::size_t offset = pos % CHAR_BIT;

    if(offset > 0) {
      *current_byte++ = (std::uint8_t)in & (0xff >> offset);

      if(n <= CHAR_BIT - offset)
	return;
      else
	n -= CHAR_BIT - offset;

      in >>= offset;
    }

    while(n >= CHAR_BIT) {
      *current_byte++ = (std::uint8_t)in & 0xff;
      in >>= 8;
      n -= CHAR_BIT;
    }

    if(n > 0)
      *current_byte = (in << (CHAR_BIT - n)) + (*current_byte >> n);
  }
  
  void
  evaluator::eval_copy(const copy_action* a)
  {
    cxt.get_diagnostics().inform(cc::location(), "Hello world");
    auto src_loc = static_cast<bitfield_expr*>(a->src);
    auto dst_loc = static_cast<bitfield_expr*>(a->dst);
    auto n = a->n->val;

    auto src_pos = static_cast<int_expr*>(src_loc->pos);
    auto src_len = static_cast<int_expr*>(src_loc->len);
    auto dst_pos = static_cast<int_expr*>(dst_loc->pos);
    auto dst_len = static_cast<int_expr*>(dst_loc->len);

    if(src_loc->as == as_key)
      throw std::runtime_error("Cannot copy from a key register.\n");
    if(dst_loc->as == as_ingress_port || dst_loc->as == as_physical_port)
      throw std::runtime_error("Cannot copy into a context variable.\n");

    if(n > dst_len->val || n > src_len->val)
      throw std::runtime_error("Copy action overflows buffer.\n");

    if(src_len->val != dst_len->val)
      throw std::runtime_error
	("Length of copy source and destination must be equal.\n");

    /// Copying into key register.
    if(dst_loc->as == as_key) {
      if(n > 64) {
	std::stringstream ss;
	ss << "Attempting to copy " << n << " bits into a register of size 64.\n";
	throw std::runtime_error(ss.str().c_str());
      }
      
      if(src_loc->as == as_packet) {	
	keyreg = data_to_key_reg((std::uint8_t*)data.data(), src_pos->val, src_len->val);
	std::cout << "value at packet: ";
	for(int i = 0; i < src_len->val / 8; ++i)
	  std::cout << (unsigned)*(data.data() + src_pos->val + i);
	std::cout << '\n';
      }

      else if(src_loc->as == as_header) {
	keyreg = data_to_key_reg((std::uint8_t*)data.data(), src_pos->val + decode, src_len->val);	
      }
      
      else if(src_loc->as == as_meta)
	keyreg = reg_to_reg(metadata, keyreg, src_pos->val, src_len->val);
      else if(src_loc->as == as_ingress_port)
	keyreg = reg_to_reg(ingress_port, keyreg, src_pos->val, src_len->val);
      else if(src_loc->as == as_physical_port)
	keyreg = reg_to_reg(physical_port, keyreg, src_pos->val, src_len->val);

      std::cout << "Copy " << n << " bits to key register from position " << src_pos->val << ". Register value: " << keyreg << '\n';
    }

    /// Copying into header bitfield.
    else if(dst_loc->as == as_header) {
      if(src_loc->as == as_packet) {
	if((dst_len->val + dst_pos->val + decode) > data.size()) {
	  std::stringstream ss;
	  ss << "Cannot write beyond buffer. Attempting to copy ";
	  ss << dst_len->val;
	  ss << " bits at position ";
	  ss << dst_pos->val;
	  ss << " of a buffer of size ";
	  ss << data.size() - decode;
	  throw std::runtime_error(ss.str().c_str());
	}
	
	bitwise_copy(modified_buffer, (std::uint8_t*)data.data(), dst_pos->val + decode, dst_len->val);
      }
      else if(src_loc->as == as_meta)
	reg_to_buf(modified_buffer, metadata, dst_pos->val + decode, dst_len->val);
      else if(src_loc->as == ingress_port)
	reg_to_buf(modified_buffer, ingress_port, dst_pos->val + decode, dst_len->val);
      else if(src_loc->as == physical_port)
	reg_to_buf(modified_buffer, physical_port, dst_pos->val + decode, dst_len->val);

      std::cout << "Copy " << dst_len->val << " bits at position " << dst_pos->val << " into header. Header value: (unimplemented)\n";
      // TODO: print(header);
    }

    /// Copying into metadata register.
    else if(dst_loc->as == as_meta) {
      if(n > 64) {
	std::stringstream ss;
	ss << "Attempting to copy " << n << " bits into a register of size 64.\n";
	throw std::runtime_error(ss.str().c_str());
      }
      
      if(src_loc->as == as_packet) {	
	metadata = data_to_key_reg((std::uint8_t*)data.data(), src_pos->val, src_len->val);
	std::cout << "value at packet: ";
	for(int i = 0; i < src_len->val / 8; ++i)
	  std::cout << (unsigned)*(data.data() + src_pos->val + i);
	std::cout << '\n';
      }

      else if(src_loc->as == as_header) {
	metadata = data_to_key_reg((std::uint8_t*)data.data(), src_pos->val + decode, src_len->val);	
      }
      
      else if(src_loc->as == as_meta)
	metadata = reg_to_reg(metadata, metadata, src_pos->val, src_len->val);
      else if(src_loc->as == as_ingress_port)
	metadata = reg_to_reg(ingress_port, metadata, src_pos->val, src_len->val);
      else if(src_loc->as == as_physical_port)
	metadata = reg_to_reg(physical_port, metadata, src_pos->val, src_len->val);


      std::cout << "Copy " << n << " bits to metadata register from position " << src_pos->val << ". Register value: " << keyreg << '\n';
    }

    /// Copying into packet bitfield.
    else if(dst_loc->as == as_packet) {
      if(src_loc->as == as_header) {
	if(dst_len->val + dst_pos->val > data.size()) {
	  std::stringstream ss;
	  ss << "Cannot write beyond buffer. Attempting to copy ";
	  ss << dst_len->val;
	  ss << " bits at position ";
	  ss << dst_pos->val;
	  ss << " of a buffer of size ";
	  ss << data.size();
	  throw std::runtime_error(ss.str().c_str());
	}
	
	bitwise_copy(modified_buffer, packet_header(data), dst_pos->val, dst_len->val);
      }
      else if(src_loc->as == as_meta) {
	if(n > data.size()) {
	  std::stringstream ss;
	  ss << "Attempting to copy " << n << " bits into a packet of size: " << data.size();
	  throw std::runtime_error(ss.str().c_str());
	}
      
	reg_to_buf(modified_buffer, metadata, dst_pos->val, dst_len->val);
      }

      else if(src_loc->as == as_ingress_port) {
	if(n > data.size()) {
	  std::stringstream ss;
	  ss << "Attempting to copy " << n << " bits into a packet of size: " << data.size();
	  throw std::runtime_error(ss.str().c_str());
	}
      
	reg_to_buf(modified_buffer, ingress_port, dst_pos->val, dst_len->val);
      }

      else if(src_loc->as == as_physical_port) {
	if(n > data.size()) {
	  std::stringstream ss;
	  ss << "Attempting to copy " << n << " bits into a packet of size: " << data.size();
	  throw std::runtime_error(ss.str().c_str());
	}
      
	reg_to_buf(modified_buffer, physical_port, dst_pos->val, dst_len->val);
      }

      std::cout << "Copy " << dst_len->val << " bits at position " << dst_pos->val << " into packet. packet value: (unimplemented)\n";
      // TODO: print(packet);
    }
  }

  void
  evaluator::eval_set(const set_action* a)
  {
    const auto loc = static_cast<bitfield_expr*>(a->f);
    const auto pos_expr = static_cast<int_expr*>(loc->pos);

    const auto val_expr = static_cast<int_expr*>(a->v);

    std::uint64_t value = val_expr->val;
    std::size_t val_width = static_cast<int_type*>(val_expr->ty)->width;    
    std::size_t position = pos_expr->val;

    if(val_width + position > data.size()) {
      std::stringstream ss;
      ss << "Cannot write beyond buffer. Attempting to write a value of ";
      ss << val_width;
      ss << " bits at position ";
      ss << position;
      ss << " of a buffer of size ";
      ss << data.size();
      throw std::runtime_error(ss.str().c_str());
    }

    reg_to_buf(modified_buffer, value, position, val_width);

    std::cout << "Set " << val_width << " bits of packet to " << value << ". Value of packet: (unimplemented).\n";
  }

  void
  evaluator::eval_write(const write_action* a)
  {
    actions.push_back(a);
    std::cout << "Write action:\n";
    dumper d(std::cout);
    d(a);
  }

  void
  evaluator::eval_clear(const clear_action* a)
  {
    eval.clear();
    std::cout << "Clear.\n";
  }

  void
  evaluator::eval_drop(const drop_action* a)
  {
    eval.clear();
    actions.clear();
    std::cout << "Drop.\n";
  }

  void
  evaluator::eval_match(const match_action* a)
  {
    // If one of the rules matches the key register, then evaluate
    // that rule's action list.

    std::cout << "keyreg: " << keyreg << '\n';
    std::cout << "keyreg ntohs: " << ntohs(keyreg) << '\n';
    
    for(auto r : current_table->rules)
    {
      auto key = r->key;

      if(get_kind(key) == ek_miss) {
      	std::cout << "packet missed.\n";
      	eval.insert(eval.end(), r->acts.begin(), r->acts.end());
      	break;
      }

      if(current_table->key_table.find(keyreg) != current_table->key_table.end()) {
	std::cout << keyreg << " was matched in table.\n";
	eval.insert(eval.end(), r->acts.begin(), r->acts.end());
	break;
      }
    }
  }


  void
  evaluator::eval_goto(const goto_action* a)
  {
    // goto is a terminator action; eval must be empty at this point for this
    // program to be well formed.
    assert(eval.empty());
    
    ref_expr* dst = static_cast<ref_expr*>(a->dest);
    current_table = static_cast<table_decl*>(dst->ref);

    for(auto a : current_table->prep)
      eval.push_back(a);

    std::cout << "Goto table: " << *(dst->id) << '\n';
  }

  void
  evaluator::eval_output(const output_action* a)
  {
    const port_expr* port = static_cast<port_expr*>(a->port);

    int port_num;
    
    if(port->rp == rp_non_reserved) {
      port_num = static_cast<int_expr*>(port->port_num)->val;
      std::cout << "Output to port: " << port_num << '\n';
    } else {
      port_num = (int)port->rp;
      std::cout << "Output to port: " << get_phrase_name(port->rp) << '\n';

      if(port_num == rp_in_port)
	std::cout << "in_port number: " << physical_port << ")\n";
      else if(port_num == rp_controller)
	controller = true;

    }
    
    egress_port = port_num;
  }

} // namespace pip
