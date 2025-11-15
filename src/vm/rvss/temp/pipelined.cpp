/**
 * @file rvss_vm.cpp
 * @brief RVSS VM implementation
 * @author Vishank Singh, https://github.com/VishankSingh
 */

#include "vm/rvss/rvss_vm.h"

#include "utils.h"
#include "globals.h"
#include "common/instructions.h"
#include "config.h"

#include <cctype>
#include <cstdint>
#include <iostream>
#include <tuple>
#include <stack>  
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

using instruction_set::Instruction;
using instruction_set::get_instr_encoding;


RVSSVM::RVSSVM() : VmBase() {
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

RVSSVM::~RVSSVM() = default;

// void RVSSVM::Fetch() {
//   if_id_write.instruction = memory_controller_.ReadWord(program_counter_);
//   if_id_write.pc = program_counter_;
//   UpdateProgramCounter(4);
// }

void RVSSVM::Fetch() {
  uint64_t current_pc = program_counter_;
  if_id_write.instruction = memory_controller_.ReadWord(current_pc);
  if_id_write.pc = current_pc;

  // Branch Prediction Logic
  if (branch_predictor_table_.count(current_pc)) {
    PredictionEntry& entry = branch_predictor_table_[current_pc];
    if (entry.taken) {
      // Predict TAKEN
      program_counter_ = entry.target_pc;
      if_id_write.predicted_taken = true;
    } else {
      // Predict NOT TAKEN
      UpdateProgramCounter(4);
      if_id_write.predicted_taken = false;
    }
  } else {
    // Not in table, default to NOT TAKEN
    UpdateProgramCounter(4);
    if_id_write.predicted_taken = false;
  }
}

void RVSSVM::Decode() {

  // clearing the write register, so that we don't have any previous instruction's data in it
  id_ex_write = {};
  
  uint32_t current_instruction = if_id_read.instruction;
  uint8_t opcode = current_instruction & 0b1111111;
  uint8_t funct3 = (current_instruction >> 12) & 0b111;
  control_unit_.SetControlSignals(current_instruction);

  if(opcode == 0b1110011 && funct3 == 0b000){ // its an ecall instruction
    // in such case I am hard coding the value of rs1 and rs2 to be x10 and x17, this automatically takes care of any hazards
    id_ex_write.rs1_num = 10; // a0
    id_ex_write.rs2_num = 17; // a7
  }
  else{
    id_ex_write.rs1_num = (current_instruction >> 15) & 0b11111;
    id_ex_write.rs2_num = (current_instruction >> 20) & 0b11111;
  }

  // reading from FPR or GPR depending upon control signal
  if (control_unit_.GetRs1IsFpr()) {
    id_ex_write.reg1_value = registers_.ReadFpr(id_ex_write.rs1_num);
  } else {
    id_ex_write.reg1_value = registers_.ReadGpr(id_ex_write.rs1_num);
  }

  if (control_unit_.GetRs2IsFpr()) {
    id_ex_write.reg2_value = registers_.ReadFpr(id_ex_write.rs2_num);
  } else {
    id_ex_write.reg2_value = registers_.ReadGpr(id_ex_write.rs2_num);
  }

  // std::cout<<id_ex_write.reg1_value<<" "<<id_ex_write.reg2_value<<std::endl;
  // id_ex_write.reg1_value = registers_.ReadGpr(id_ex_write.rs1_num);
  // id_ex_write.reg2_value = registers_.ReadGpr(id_ex_write.rs2_num);


  // forwarding data
  id_ex_write.pc = if_id_read.pc;
  id_ex_write.instruction = if_id_read.instruction;
  id_ex_write.predicted_taken = if_id_read.predicted_taken;

  id_ex_write.imm = ImmGenerator(current_instruction);
  id_ex_write.rd_num = (current_instruction >> 7) & 0b11111;
      
  id_ex_write.alu_op_ = control_unit_.GetAluOp();
  id_ex_write.reg_write = control_unit_.GetRegWrite();
  id_ex_write.mem_read = control_unit_.GetMemRead();
  id_ex_write.mem_write = control_unit_.GetMemWrite();
  id_ex_write.mem_to_reg = control_unit_.GetMemToReg();
  id_ex_write.alu_src = control_unit_.GetAluSrc();
  id_ex_write.branch = control_unit_.GetBranch();  
  id_ex_write.rd_is_fpr = control_unit_.GetRdIsFpr(); // passing, if we are writing the result to FPR or not
  id_ex_write.rs1_is_fpr = control_unit_.GetRs1IsFpr();
  id_ex_write.rs2_is_fpr = control_unit_.GetRs2IsFpr();
}

void RVSSVM::Execute() {
  // a
  execution_result_ = 0;
  // clearing the write register, so that we don't have any previous instruction's data in it
  ex_mem_write = {};

  uint32_t current_instruction = id_ex_read.instruction;
  uint8_t opcode = current_instruction & 0b1111111;
  uint8_t funct3 = (current_instruction >> 12) & 0b111;
  
  if (opcode == get_instr_encoding(Instruction::kecall).opcode && 
  funct3 == get_instr_encoding(Instruction::kecall).funct3) {
    HandleSyscall();
    return;
  }

  if (instruction_set::isFInstruction(current_instruction)) { // RV64 F
    ExecuteFloat();
    return;
  } else if (instruction_set::isDInstruction(current_instruction)) {
    ExecuteDouble();
    // return;
  } else if (opcode==0b1110011) {
    ExecuteCsr();
    return;
  }

  // uint8_t rs1 = (current_instruction >> 15) & 0b11111;
  // uint8_t rs2 = (current_instruction >> 20) & 0b11111;
  
  // a
  uint8_t rs1 = id_ex_read.rs1_num;
  uint8_t rs2 = id_ex_read.rs2_num;
  
  // int32_t imm = ImmGenerator(current_instruction);
  
  // a
  int32_t imm = id_ex_read.imm;

  // uint64_t reg1_value = registers_.ReadGpr(rs1);
  // uint64_t reg2_value = registers_.ReadGpr(rs2);
  
  // a
  uint64_t reg1_value = id_ex_read.reg1_value;
  uint64_t reg2_value = id_ex_read.reg2_value;

  bool overflow = false;

  if (id_ex_read.alu_src) {
    reg2_value = static_cast<uint64_t>(static_cast<int64_t>(imm));
  }

  // previously LUI 12 bit shift was done in the writeback stage, but it will give wrong answer if we forward it from ex_mem stage
  // so now we are directly shifting it here
  if (opcode == 0b0110111) { // LUI
      reg1_value = 0;
      reg2_value = static_cast<uint64_t>(static_cast<int64_t>(imm) << 12);
  }

  alu::AluOp aluOperation = control_unit_.GetAluSignal(id_ex_read.instruction, id_ex_read.alu_op_);
  std::tie(execution_result_, overflow) = alu_.execute(aluOperation, reg1_value, reg2_value);

  std::cout<< "PC =====> "<< id_ex_read.pc <<std::endl;
  std::cerr << "ALU Operation: " << aluOperation <<" "<<reg1_value<<" "<<reg2_value<< std::endl;
  std::cout<<execution_result_<<std::endl;


  uint64_t actual_target_pc = 0;

  if (id_ex_read.branch) {
    if (opcode==get_instr_encoding(Instruction::kjalr).opcode || 
        opcode==get_instr_encoding(Instruction::kjal).opcode) {
      
      // a
      branch_flag_ = true;

      // next_pc_ = static_cast<int64_t>(id_ex_read.pc+4); // next insruction is at PC+4

      // UpdateProgramCounter(-8); // changed it to -8 from -4, because there is one instruction fetched after Jal one, so the PC has moved twice

      return_address_ = id_ex_read.pc + 4;
      if (opcode==get_instr_encoding(Instruction::kjalr).opcode) { 
        // UpdateProgramCounter(-id_ex_read.pc + (execution_result_));
        actual_target_pc = program_counter_;
        actual_target_pc = static_cast<uint64_t>(execution_result_);
        // UpdateProgramCounter(+id_ex_read.pc - (execution_result_));
      } else if (opcode==get_instr_encoding(Instruction::kjal).opcode) {
        // UpdateProgramCounter(imm);
        actual_target_pc = static_cast<uint64_t>(static_cast<int64_t>(id_ex_read.pc)+imm);
        // UpdateProgramCounter(-imm);
      }
    } else if (opcode==get_instr_encoding(Instruction::kbeq).opcode ||
               opcode==get_instr_encoding(Instruction::kbne).opcode ||
               opcode==get_instr_encoding(Instruction::kblt).opcode ||
               opcode==get_instr_encoding(Instruction::kbge).opcode ||
               opcode==get_instr_encoding(Instruction::kbltu).opcode ||
               opcode==get_instr_encoding(Instruction::kbgeu).opcode) {
      switch (funct3) {
        case 0b000: {// BEQ
          branch_flag_ = (execution_result_==0);
          break;
        }
        case 0b001: {// BNE
          branch_flag_ = (execution_result_!=0);
          break;
        }
        case 0b100: {// BLT
          branch_flag_ = (execution_result_==1);
          break;
        }
        case 0b101: {// BGE
          branch_flag_ = (execution_result_==0);
          break;
        }
        case 0b110: {// BLTU
          branch_flag_ = (execution_result_==1);
          break;
        }
        case 0b111: {// BGEU
          branch_flag_ = (execution_result_==0);
          break;
        }
      }
    }
  }

  // if branch taken, then move the PC to its correct location
  if (branch_flag_ && opcode==0b1100011) {

    // a -> changed it to -8
    // UpdateProgramCounter(-8);
    // UpdateProgramCounter(imm);
    actual_target_pc = static_cast<uint64_t>(static_cast<int64_t>(id_ex_read.pc) + imm);
    // a
    // ex_mem_write.branch_target_pc = program_counter_;
  }


  if (opcode==get_instr_encoding(Instruction::kauipc).opcode) { // AUIPC
    execution_result_ = static_cast<int64_t>(program_counter_) - 4 + (imm << 12);
  }


  // implementing the correction unit in hazard detection
  ex_mem_write.actual_taken = branch_flag_;
  ex_mem_write.actual_target_pc = actual_target_pc;
  ex_mem_write.is_branch = id_ex_read.branch;
  ex_mem_write.predicted_taken = id_ex_read.predicted_taken;

  // a
  // adding new data to intermediate register
  ex_mem_write.alu_result = execution_result_;
  ex_mem_write.branch_taken = branch_flag_;
  ex_mem_write.next_pc = return_address_;
  

  // a set the signal to default after using it
  branch_flag_ = false;
  return_address_ = 0;

  // std::cout<<"hello"<<ex_mem_register.alu_result<<std::endl;

  // forwarding the current values from the intermediate registers
  ex_mem_write.pc = id_ex_read.pc;
  ex_mem_write.instruction = id_ex_read.instruction;
  ex_mem_write.rd_num = id_ex_read.rd_num;
  ex_mem_write.reg2_value = id_ex_read.reg2_value;
  ex_mem_write.reg_write = id_ex_read.reg_write;
  ex_mem_write.mem_read = id_ex_read.mem_read;
  ex_mem_write.mem_write = id_ex_read.mem_write;
  ex_mem_write.mem_to_reg = id_ex_read.mem_to_reg;
  ex_mem_write.rd_is_fpr = id_ex_read.rd_is_fpr;
  ex_mem_write.rs1_is_fpr = id_ex_read.rs1_is_fpr;
  ex_mem_write.rs2_is_fpr = id_ex_read.rs2_is_fpr;
}

void RVSSVM::ExecuteFloat() {
  // std::cout<<"executing float"<<std::endl;
  uint64_t current_instruction = id_ex_read.instruction;
  uint8_t opcode = current_instruction & 0b1111111;
  uint8_t funct3 = (current_instruction >> 12) & 0b111;
  uint8_t funct7 = (current_instruction >> 25) & 0b1111111;
  uint8_t rm = funct3;
  uint8_t rs1 = (current_instruction >> 15) & 0b11111;
  uint8_t rs2 = (current_instruction >> 20) & 0b11111;
  uint8_t rs3 = (current_instruction >> 27) & 0b11111;

  uint8_t fcsr_status = 0;

  int32_t imm = ImmGenerator(current_instruction);

  if (rm==0b111) { // not doing it now, CSR later
    rm = registers_.ReadCsr(0x002);
  }

  // data hazard is not dealt with
  uint64_t reg1_value = id_ex_read.reg1_value;
  uint64_t reg2_value = id_ex_read.reg2_value;
  uint64_t reg3_value = registers_.ReadFpr(rs3); // no hazard detection for r3, directly reading it from register file

  if (funct7==0b1101000 || funct7==0b1111000 || opcode==0b0000111 || opcode==0b0100111) { // in this case rs1 if from GPR and rs2 from FPR
    // reg1_value = registers_.ReadGpr(rs1); // we are directly reading from register file
  }

  if (control_unit_.GetAluSrc()) {
    reg2_value = static_cast<uint64_t>(static_cast<int64_t>(imm));
  }

  
  alu::AluOp aluOperation = control_unit_.GetAluSignal(current_instruction, id_ex_read.alu_op_);
  std::tie(execution_result_, fcsr_status) = alu::Alu::fpexecute(aluOperation, reg1_value, reg2_value, reg3_value, rm);
  
  std::cout<< "PC =====> "<< id_ex_read.pc <<std::endl;
  std::cerr << "ALU Operation: " << aluOperation <<" "<<reg1_value<<" "<<reg2_value<< std::endl;
  std::cout<<execution_result_<<std::endl;


  // std::cout<<aluOperation<<std::endl;
  // std::cout<<reg1_value<<" "<<reg2_value<<" "<<reg3_value<<std::endl;
  // std::cout << "execution result: " << execution_result_ << std::endl;

  // a
  // implementing the correction unit in hazard detection
  ex_mem_write.is_branch = id_ex_read.branch;
  ex_mem_write.predicted_taken = id_ex_read.predicted_taken;

  // adding new data to intermediate register
  ex_mem_write.alu_result = execution_result_;
  ex_mem_write.branch_taken = branch_flag_;
  ex_mem_write.next_pc = return_address_;
  

  // a set the signal to default after using it
  branch_flag_ = false;

  // std::cout<<"hello"<<ex_mem_register.alu_result<<std::endl;

  // forwarding the current values from the intermediate registers
  ex_mem_write.pc = id_ex_read.pc;
  ex_mem_write.instruction = id_ex_read.instruction;
  ex_mem_write.rd_num = id_ex_read.rd_num;
  ex_mem_write.reg2_value = id_ex_read.reg2_value;
  ex_mem_write.reg_write = id_ex_read.reg_write;
  ex_mem_write.mem_read = id_ex_read.mem_read;
  ex_mem_write.mem_write = id_ex_read.mem_write;
  ex_mem_write.mem_to_reg = id_ex_read.mem_to_reg; 
  ex_mem_write.rd_is_fpr = id_ex_read.rd_is_fpr;
  ex_mem_write.rs1_is_fpr = id_ex_read.rs1_is_fpr;
  ex_mem_write.rs2_is_fpr = id_ex_read.rs2_is_fpr;

  registers_.WriteCsr(0x003, fcsr_status);
}

void RVSSVM::ExecuteDouble() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;
  uint8_t funct7 = (current_instruction_ >> 25) & 0b1111111;
  uint8_t rm = funct3;
  uint8_t rs1 = (current_instruction_ >> 15) & 0b11111;
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;
  uint8_t rs3 = (current_instruction_ >> 27) & 0b11111;

  uint8_t fcsr_status = 0;

  int32_t imm = ImmGenerator(current_instruction_);

  uint64_t reg1_value = registers_.ReadFpr(rs1);
  uint64_t reg2_value = registers_.ReadFpr(rs2);
  uint64_t reg3_value = registers_.ReadFpr(rs3);

  if (funct7==0b1101001 || funct7==0b1111001 || opcode==0b0000111 || opcode==0b0100111) {
    reg1_value = registers_.ReadGpr(rs1);
  }

  if (control_unit_.GetAluSrc()) {
    reg2_value = static_cast<uint64_t>(static_cast<int64_t>(imm));
  }

  alu::AluOp aluOperation = control_unit_.GetAluSignal(current_instruction_, control_unit_.GetAluOp());
  std::tie(execution_result_, fcsr_status) = alu::Alu::dfpexecute(aluOperation, reg1_value, reg2_value, reg3_value, rm);
}

void RVSSVM::ExecuteCsr() {
  uint8_t rs1 = (current_instruction_ >> 15) & 0b11111;
  uint16_t csr = (current_instruction_ >> 20) & 0xFFF;
  uint64_t csr_val = registers_.ReadCsr(csr);

  csr_target_address_ = csr;
  csr_old_value_ = csr_val;
  csr_write_val_ = registers_.ReadGpr(rs1);
  csr_uimm_ = rs1;
}

// for the current implementation syscall READ and WRITE may have data hazard in its implementation, 
// as they are still directly reading from teh register file
void RVSSVM::HandleSyscall() {
  // uint64_t syscall_number = registers_.ReadGpr(17);

  // a
  uint64_t syscall_number = id_ex_read.reg2_value;
  uint64_t argument = id_ex_read.reg1_value;

  switch (syscall_number) {
    case SYSCALL_PRINT_INT: {
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        } else {
          std::cout << "VM_STDOUT_START";
        }
        std::cout << static_cast<int64_t>(argument); // Print signed integer
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        } else {
          std::cout << "VM_STDOUT_END" << std::endl;
        }
        break;
    }
    case SYSCALL_PRINT_FLOAT: { // print float
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        } else {
          std::cout << "VM_STDOUT_START";
        }
        float float_value;
        uint64_t raw = argument;
        std::memcpy(&float_value, &raw, sizeof(float_value));
        std::cout << std::setprecision(std::numeric_limits<float>::max_digits10) << float_value;
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        } else {
          std::cout << "VM_STDOUT_END" << std::endl;
        }
        break;
    }
    case SYSCALL_PRINT_DOUBLE: { // print double
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        } else {
          std::cout << "VM_STDOUT_START";
        }
        double double_value;
        uint64_t raw = argument;
        std::memcpy(&double_value, &raw, sizeof(double_value));
        std::cout << std::setprecision(std::numeric_limits<double>::max_digits10) << double_value;
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        } else {
          std::cout << "VM_STDOUT_END" << std::endl;
        }
        break;
    }
    case SYSCALL_PRINT_STRING: {
        if (!globals::vm_as_backend) {
            std::cout << "[Syscall output: ";
        }
        PrintString(argument); // Print string
        if (!globals::vm_as_backend) {
            std::cout << "]" << std::endl;
        }
        break;
    }
    case SYSCALL_EXIT: {
        stop_requested_ = true; // Stop the VM
        if (!globals::vm_as_backend) {
            std::cout << "VM_EXIT" << std::endl;
        }
        output_status_ = "VM_EXIT";
        std::cout << "Exited with exit code: " << argument << std::endl;
        exit(0); // Exit the program
        break;
    }
    case SYSCALL_READ: { // Read
      uint64_t file_descriptor = argument;
      uint64_t buffer_address = registers_.ReadGpr(11);
      uint64_t length = registers_.ReadGpr(12);

      if (file_descriptor == 0) {
        // Read from stdin
        std::string input;
        {
          std::cout << "VM_STDIN_START" << std::endl;
          output_status_ = "VM_STDIN_START";
          std::unique_lock<std::mutex> lock(input_mutex_);
          input_cv_.wait(lock, [this]() { 
            return !input_queue_.empty(); 
          });
          output_status_ = "VM_STDIN_END";
          std::cout << "VM_STDIN_END" << std::endl;

          input = input_queue_.front();
          input_queue_.pop();
        }


        std::vector<uint8_t> old_bytes_vec(length, 0);
        std::vector<uint8_t> new_bytes_vec(length, 0);

        for (size_t i = 0; i < length; ++i) {
          old_bytes_vec[i] = memory_controller_.ReadByte(buffer_address + i);
        }
        
        for (size_t i = 0; i < input.size() && i < length; ++i) {
          memory_controller_.WriteByte(buffer_address + i, static_cast<uint8_t>(input[i]));
        }
        if (input.size() < length) {
          memory_controller_.WriteByte(buffer_address + input.size(), '\0');
        }

        for (size_t i = 0; i < length; ++i) {
          new_bytes_vec[i] = memory_controller_.ReadByte(buffer_address + i);
        }

        current_delta_.memory_changes.push_back({
          buffer_address, 
          old_bytes_vec, 
          new_bytes_vec
        });

        uint64_t old_reg = argument;
        unsigned int reg_index = 10;
        unsigned int reg_type = 0; // 0 for GPR, 1 for CSR, 2 for FPR
        uint64_t new_reg = std::min(static_cast<uint64_t>(length), static_cast<uint64_t>(input.size()));
        registers_.WriteGpr(10, new_reg); 
        if (old_reg != new_reg) {
          current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
        }

      } else {
          std::cerr << "Unsupported file descriptor: " << file_descriptor << std::endl;
      }
      break;
    }
    case SYSCALL_WRITE: { // Write
        uint64_t file_descriptor = argument;
        uint64_t buffer_address = registers_.ReadGpr(11);
        uint64_t length = registers_.ReadGpr(12);

        if (file_descriptor == 1) { // stdout
          std::cout << "VM_STDOUT_START";
          output_status_ = "VM_STDOUT_START";
          uint64_t bytes_printed = 0;
          for (uint64_t i = 0; i < length; ++i) {
              char c = memory_controller_.ReadByte(buffer_address + i);
              // if (c == '\0') {
              //     break;
              // }
              std::cout << c;
              bytes_printed++;
          }
          std::cout << std::flush; 
          output_status_ = "VM_STDOUT_END";
          std::cout << "VM_STDOUT_END" << std::endl;

          uint64_t old_reg = argument;
          unsigned int reg_index = 10;
          unsigned int reg_type = 0; // 0 for GPR, 1 for CSR, 2 for FPR
          uint64_t new_reg = std::min(static_cast<uint64_t>(length), bytes_printed);
          registers_.WriteGpr(10, new_reg);
          if (old_reg != new_reg) {
            current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
          }
        } else {
            std::cerr << "Unsupported file descriptor: " << file_descriptor << std::endl;
        }
        break;
    }
    default: {
      std::cerr << "Unknown syscall number: " << syscall_number << std::endl;
      break;
    }
  }
}

// reads from ex_mem_register and writes to mem_wb_register
void RVSSVM::WriteMemory() {
  // a
  memory_result_ = 0;
  // clearing the write register, so that we don't have any previous instruction's data in it
  mem_wb_write = {};

  uint32_t current_instruction = ex_mem_read.instruction;
  uint64_t execution_result = ex_mem_read.alu_result;
  uint8_t opcode = current_instruction & 0b1111111;
  uint8_t rs2 = (current_instruction >> 20) & 0b11111;
  uint8_t funct3 = (current_instruction >> 12) & 0b111;

  if (opcode == 0b1110011 && funct3 == 0b000) {
    return;
  }

  if (instruction_set::isFInstruction(current_instruction)) { // RV64 F
    WriteMemoryFloat();
    return;
  } else if (instruction_set::isDInstruction(current_instruction)) {
    WriteMemoryDouble();
    return;
  }

  if (ex_mem_read.mem_read) {
    switch (funct3) {
      case 0b000: {// LB
        memory_result_ = static_cast<int8_t>(memory_controller_.ReadByte(execution_result));
        break;
      }
      case 0b001: {// LH
        memory_result_ = static_cast<int16_t>(memory_controller_.ReadHalfWord(execution_result));
        break;
      }
      case 0b010: {// LW
        memory_result_ = static_cast<int32_t>(memory_controller_.ReadWord(execution_result));
        break;
      }
      case 0b011: {// LD
        memory_result_ = memory_controller_.ReadDoubleWord(execution_result);
        break;
      }
      case 0b100: {// LBU
        memory_result_ = static_cast<uint8_t>(memory_controller_.ReadByte(execution_result));
        break;
      }
      case 0b101: {// LHU
        memory_result_ = static_cast<uint16_t>(memory_controller_.ReadHalfWord(execution_result));
        break;
      }
      case 0b110: {// LWU
        memory_result_ = static_cast<uint32_t>(memory_controller_.ReadWord(execution_result));
        break;
      }
    }
  }

  uint64_t addr = 0;
  std::vector<uint8_t> old_bytes_vec;
  std::vector<uint8_t> new_bytes_vec;

  // TODO: use direct read to read memory for undo/redo functionality, i.e. ReadByte -> ReadByte_d


  if (ex_mem_read.mem_write) {
    switch (funct3) {
      case 0b000: {// SB
        addr = execution_result;
        old_bytes_vec.push_back(memory_controller_.ReadByte(addr));
        memory_controller_.WriteByte(execution_result, ex_mem_read.reg2_value & 0xFF);
        new_bytes_vec.push_back(memory_controller_.ReadByte(addr));
        break;
      }
      case 0b001: {// SH
        addr = execution_result;
        for (size_t i = 0; i < 2; ++i) {
          old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        memory_controller_.WriteHalfWord(execution_result, ex_mem_read.reg2_value & 0xFFFF);
        for (size_t i = 0; i < 2; ++i) {
          new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        break;
      }
      case 0b010: {// SW
        addr = execution_result;
        for (size_t i = 0; i < 4; ++i) {
          old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        memory_controller_.WriteWord(execution_result, ex_mem_read.reg2_value & 0xFFFFFFFF);
        for (size_t i = 0; i < 4; ++i) {
          new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        break;
      }
      case 0b011: {// SD
        std::cout<<execution_result<<std::endl;
        addr = execution_result;
        for (size_t i = 0; i < 8; ++i) {
          old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        memory_controller_.WriteDoubleWord(execution_result, ex_mem_read.reg2_value & 0xFFFFFFFFFFFFFFFF);
        for (size_t i = 0; i < 8; ++i) {
          new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
        }
        break;
      }
    }
  }

  if (old_bytes_vec != new_bytes_vec) {
    current_delta_.memory_changes.push_back({
      addr,
      old_bytes_vec,
      new_bytes_vec
    });
  }

  // adding new data to mem_wb_register
  mem_wb_write.memory_read_data = memory_result_;

  // forwarding the data from ex_mem_register to mem_wb_register
  mem_wb_write.pc = ex_mem_read.pc;
  mem_wb_write.next_pc = ex_mem_read.next_pc;
  mem_wb_write.alu_result = ex_mem_read.alu_result;
  mem_wb_write.instruction = ex_mem_read.instruction;
  mem_wb_write.rd_num = ex_mem_read.rd_num;
  mem_wb_write.reg_write = ex_mem_read.reg_write;
  mem_wb_write.mem_to_reg = ex_mem_read.mem_to_reg;
  mem_wb_write.rd_is_fpr = ex_mem_read.rd_is_fpr;
  mem_wb_write.rs1_is_fpr = ex_mem_read.rs1_is_fpr;
  mem_wb_write.rs2_is_fpr = ex_mem_read.rs2_is_fpr;
}

void RVSSVM::WriteMemoryFloat() {
  // std::cout<<"write memory float"<<std::endl;
  uint64_t current_instruction = ex_mem_read.instruction;
  uint8_t rs2 = (current_instruction >> 20) & 0b11111;

  if (ex_mem_read.mem_read) { // FLW
    memory_result_ = memory_controller_.ReadWord(ex_mem_read.alu_result);
  }

  // std::cout << "+++++ Memory result: " << memory_result_ << std::endl;

  uint64_t addr = 0;
  std::vector<uint8_t> old_bytes_vec;
  std::vector<uint8_t> new_bytes_vec;

  if (ex_mem_read.mem_write) { // FSW
    addr = ex_mem_read.alu_result;
    for (size_t i = 0; i < 4; ++i) {
      old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
    uint32_t val = ex_mem_read.reg2_value & 0xFFFFFFFF;
    memory_controller_.WriteWord(ex_mem_read.alu_result, val);
    // new_bytes_vec.push_back(memory_controller_.ReadByte(addr));
    for (size_t i = 0; i < 4; ++i) {
      new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
  }

  if (old_bytes_vec!=new_bytes_vec) {
    current_delta_.memory_changes.push_back({addr, old_bytes_vec, new_bytes_vec});
  }

  // adding new data to mem_wb_register
  mem_wb_write.memory_read_data = memory_result_;

  // forwarding the data from ex_mem_register to mem_wb_register
  mem_wb_write.pc = ex_mem_read.pc;
  mem_wb_write.next_pc = ex_mem_read.next_pc;
  mem_wb_write.alu_result = ex_mem_read.alu_result;
  mem_wb_write.instruction = ex_mem_read.instruction;
  mem_wb_write.rd_num = ex_mem_read.rd_num;
  mem_wb_write.reg_write = ex_mem_read.reg_write;
  mem_wb_write.mem_to_reg = ex_mem_read.mem_to_reg;
  mem_wb_write.rd_is_fpr = ex_mem_read.rd_is_fpr;
  mem_wb_write.rs1_is_fpr = ex_mem_read.rs1_is_fpr;
  mem_wb_write.rs2_is_fpr = ex_mem_read.rs2_is_fpr;
}

void RVSSVM::WriteMemoryDouble() {
  uint8_t rs2 = (current_instruction_ >> 20) & 0b11111;

  if (control_unit_.GetMemRead()) {// FLD
    memory_result_ = memory_controller_.ReadDoubleWord(execution_result_);
  }

  uint64_t addr = 0;
  std::vector<uint8_t> old_bytes_vec;
  std::vector<uint8_t> new_bytes_vec;

  if (control_unit_.GetMemWrite()) {// FSD
    addr = execution_result_;
    for (size_t i = 0; i < 8; ++i) {
      old_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
    memory_controller_.WriteDoubleWord(execution_result_, registers_.ReadFpr(rs2));
    for (size_t i = 0; i < 8; ++i) {
      new_bytes_vec.push_back(memory_controller_.ReadByte(addr + i));
    }
  }

  if (old_bytes_vec!=new_bytes_vec) {
    current_delta_.memory_changes.push_back({addr, old_bytes_vec, new_bytes_vec});
  }
}

void RVSSVM::WriteBack() {
  
  // std::cout<<"writing back"<<std::endl;

  uint32_t current_instruction = mem_wb_read.instruction;
  uint8_t opcode = current_instruction & 0b1111111;
  uint8_t funct3 = (current_instruction >> 12) & 0b111;
  uint8_t rd = (current_instruction >> 7) & 0b11111;
  int32_t imm = ImmGenerator(current_instruction);


  if (opcode == get_instr_encoding(Instruction::kecall).opcode && 
      funct3 == get_instr_encoding(Instruction::kecall).funct3) { // ecall
    return;
  }

  if (instruction_set::isFInstruction(current_instruction)) { // RV64 F
    WriteBackFloat();
    return;
  } else if (instruction_set::isDInstruction(current_instruction)) {
    WriteBackDouble();
    return;
  } else if (opcode==0b1110011) { // CSR opcode
    WriteBackCsr();
    return;
  }

  uint64_t old_reg = registers_.ReadGpr(rd);
  unsigned int reg_index = rd;
  unsigned int reg_type = 0; // 0 for GPR, 1 for CSR, 2 for FPR


  if (mem_wb_read.reg_write) { 
    switch (opcode) {
      case get_instr_encoding(Instruction::kRtype).opcode: /* R-Type */
      case get_instr_encoding(Instruction::kItype).opcode: /* I-Type */
      case get_instr_encoding(Instruction::kauipc).opcode: /* AUIPC */ {
        registers_.WriteGpr(mem_wb_read.rd_num, mem_wb_read.alu_result);
        break;
      }
      case get_instr_encoding(Instruction::kLoadType).opcode: /* Load */ { 
        registers_.WriteGpr(mem_wb_read.rd_num, mem_wb_read.memory_read_data);
        break;
      }
      case get_instr_encoding(Instruction::kjalr).opcode: /* JALR */
      case get_instr_encoding(Instruction::kjal).opcode: /* JAL */ {
        registers_.WriteGpr(mem_wb_read.rd_num, mem_wb_read.next_pc);
        break;
      }
      case get_instr_encoding(Instruction::klui).opcode: /* LUI */ {
        registers_.WriteGpr(mem_wb_read.rd_num, mem_wb_read.alu_result); // no need of shifing 12 bits here as we have already done it
        break;
      }
      default: break;
    }
  }

  if (opcode==get_instr_encoding(Instruction::kjal).opcode) /* JAL */ {
    // Updated in Execute()
  }
  if (opcode==get_instr_encoding(Instruction::kjalr).opcode) /* JALR */ {
    // registers_.WriteGpr(rd, return_address_); // Write back to rs1
    // Updated in Execute()
  }

  uint64_t new_reg = registers_.ReadGpr(rd);
  if (old_reg!=new_reg) {
    current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
  }

}

void RVSSVM::WriteBackFloat() {
  // std::cout<<"write back float"<<std::endl;
  uint64_t current_instruction = mem_wb_read.instruction;
  uint8_t opcode = current_instruction & 0b1111111;
  uint8_t funct7 = (current_instruction >> 25) & 0b1111111;
  uint8_t rd = (current_instruction >> 7) & 0b11111;

  uint64_t old_reg = 0;
  unsigned int reg_index = rd;
  unsigned int reg_type = 2; // 0 for GPR, 1 for CSR, 2 for FPR
  uint64_t new_reg = 0;

  if (mem_wb_read.reg_write) {
    switch(funct7) {
      // write to GPR
      case get_instr_encoding(Instruction::kfle_s).funct7: // f(eq|lt|le).s
      case get_instr_encoding(Instruction::kfcvt_w_s).funct7: // fcvt.(w|wu|l|lu).s
      case get_instr_encoding(Instruction::kfmv_x_w).funct7: // fmv.x.w , fclass.s
      {
        old_reg = registers_.ReadGpr(rd);
        registers_.WriteGpr(rd, mem_wb_read.alu_result);
        new_reg = mem_wb_read.alu_result;
        reg_type = 0; // GPR
        break;
      }

      // write to FPR
      default: {
        switch (opcode) {
          case get_instr_encoding(Instruction::kflw).opcode: {
            old_reg = registers_.ReadFpr(rd);
            registers_.WriteFpr(rd, mem_wb_read.memory_read_data);
            new_reg = mem_wb_read.memory_read_data;
            reg_type = 2; // FPR
            break;
          }

          default: {
            old_reg = registers_.ReadFpr(rd);
            registers_.WriteFpr(rd, mem_wb_read.alu_result);
            new_reg = mem_wb_read.alu_result;
            reg_type = 2; // FPR
            break;
          }
        }
      }
    }

    // // write to GPR
    // if (funct7==0b1010000
    //     || funct7==0b1100000
    //     || funct7==0b1110000) { // f(eq|lt|le).s, fcvt.(w|wu|l|lu).s
    //   old_reg = registers_.ReadGpr(rd);
    //   registers_.WriteGpr(rd, execution_result_);
    //   new_reg = execution_result_;
    //   reg_type = 0; // GPR

    // }
    // // write to FPR
    // else if (opcode==get_instr_encoding(Instruction::kflw).opcode) {
    //   old_reg = registers_.ReadFpr(rd);
    //   registers_.WriteFpr(rd, memory_result_);
    //   new_reg = memory_result_;
    //   reg_type = 2; // FPR
    // } else {
    //   old_reg = registers_.ReadFpr(rd);
    //   registers_.WriteFpr(rd, execution_result_);
    //   new_reg = execution_result_;
    //   reg_type = 2; // FPR
    // }
  }

  if (old_reg!=new_reg) {
    current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
  }
}

void RVSSVM::WriteBackDouble() {
  uint8_t opcode = current_instruction_ & 0b1111111;
  uint8_t funct7 = (current_instruction_ >> 25) & 0b1111111;
  uint8_t rd = (current_instruction_ >> 7) & 0b11111;

  uint64_t old_reg = 0;
  unsigned int reg_index = rd;
  unsigned int reg_type = 2; // 0 for GPR, 1 for CSR, 2 for FPR
  uint64_t new_reg = 0;

  if (control_unit_.GetRegWrite()) {
    // write to GPR
    if (funct7==0b1010001
        || funct7==0b1100001
        || funct7==0b1110001) { // f(eq|lt|le).d, fcvt.(w|wu|l|lu).d
      old_reg = registers_.ReadGpr(rd);
      registers_.WriteGpr(rd, execution_result_);
      new_reg = execution_result_;
      reg_type = 0; // GPR
    }
      // write to FPR
    else if (opcode==0b0000111) {
      old_reg = registers_.ReadFpr(rd);
      registers_.WriteFpr(rd, memory_result_);
      new_reg = memory_result_;
      reg_type = 2; // FPR
    } else {
      old_reg = registers_.ReadFpr(rd);
      registers_.WriteFpr(rd, execution_result_);
      new_reg = execution_result_;
      reg_type = 2; // FPR
    }
  }

  if (old_reg!=new_reg) {
    current_delta_.register_changes.push_back({reg_index, reg_type, old_reg, new_reg});
  }

  return;
}

void RVSSVM::WriteBackCsr() {
  uint8_t rd = (current_instruction_ >> 7) & 0b11111;
  uint8_t funct3 = (current_instruction_ >> 12) & 0b111;

  switch (funct3) {
    case get_instr_encoding(Instruction::kcsrrw).funct3: { // CSRRW
      registers_.WriteGpr(rd, csr_old_value_);
      registers_.WriteCsr(csr_target_address_, csr_write_val_);
      break;
    }
    case get_instr_encoding(Instruction::kcsrrs).funct3: { // CSRRS
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_write_val_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ | csr_write_val_);
      }
      break;
    }
    case get_instr_encoding(Instruction::kcsrrc).funct3: { // CSRRC
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_write_val_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ & ~csr_write_val_);
      }
      break;
    }
    case get_instr_encoding(Instruction::kcsrrwi).funct3: { // CSRRWI
      registers_.WriteGpr(rd, csr_old_value_);
      registers_.WriteCsr(csr_target_address_, csr_uimm_);
      break;
    }
    case get_instr_encoding(Instruction::kcsrrsi).funct3: { // CSRRSI
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_uimm_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ | csr_uimm_);
      }
      break;
    }
    case get_instr_encoding(Instruction::kcsrrci).funct3: { // CSRRCI
      registers_.WriteGpr(rd, csr_old_value_);
      if (csr_uimm_!=0) {
        registers_.WriteCsr(csr_target_address_, csr_old_value_ & ~csr_uimm_);
      }
      break;
    }
  }

}



// a
void RVSSVM::HazardDetectionUnit(){

  // setting all local control signals to false 
  stall = forward_from_ex_mem = forward_from_mem_wb = load_use_hazard = jal_jalr_hazard = false; 

  // check for control hazard
  if(ex_mem_write.is_branch){
    if(ex_mem_write.actual_taken == ex_mem_write.predicted_taken){
      std::cout<<"Right Prediction"<<std::endl;
      // prediction is correct
      // return;
    }
    else{
      // our prediction is wrong
      std::cout<<"Wrong Prediction"<<std::endl;

      // PredictionEntry& entry = branch_predictor_table_[ex_mem_write.pc];
      // entry.taken = ex_mem_write.actual_taken;
      // if (ex_mem_write.actual_taken) {
      //   entry.target_pc = ex_mem_read.actual_target_pc; // Store the target
      // }

      id_ex_write = {};
      if_id_write = {};

      if(ex_mem_write.actual_taken){
        program_counter_ = ex_mem_write.actual_target_pc;
      }
      else{
        program_counter_ = ex_mem_write.pc+4;
      }
    }
    ex_mem_write.branch_taken = false;
    branch_predictor_table_[ex_mem_write.pc] = {ex_mem_write.actual_taken, ex_mem_write.actual_target_pc};
    // ex_mem_write.is_branch = false;
    // return;
  }
  // if(ex_mem_write.branch_taken){
  //   std::cout<<"branch taken"<<std::endl;
  //   // if branch is taken then we have to flush only second stage 
  //   // and run next iteration from new PC
    
  //   // if_id_write = {}; // we dont need this as the instruction fetched after running the excute stage 
  //   // will be from the taken branch so its correct instruction, no need to flush it
    
  //   // UpdateProgramCounter(-4);
  //   stall = true;
  //   id_ex_write = {};
  //   ex_mem_write.branch_taken = false;
  //   return;
  // }

  // implement differencing b/w GPR and FPR

  // check for hazards and change control signals accordingly
  if(ex_mem_write.reg_write && ((!ex_mem_write.rd_is_fpr && ex_mem_write.rd_num != 0) || ex_mem_write.rd_is_fpr)
    && ((id_ex_write.rs1_num == ex_mem_write.rd_num && id_ex_write.rs1_is_fpr == ex_mem_write.rd_is_fpr) 
      || (id_ex_write.rs2_num == ex_mem_write.rd_num && id_ex_write.rs2_is_fpr == ex_mem_write.rd_is_fpr))){
    
    // stall = true;
    
    if(ex_mem_write.mem_read){ // its a load instruction, we must add NOP
      // adding NOP
      // UpdateProgramCounter(-4);
      if_id_write.pc = id_ex_write.pc;
      if_id_write.instruction = id_ex_write.instruction;
      stall = true;
      id_ex_write = {};
      return;
    }
    else{
      forward_from_ex_mem = true;
    }
  } 

  if(mem_wb_write.reg_write && ((!mem_wb_write.rd_is_fpr && mem_wb_write.rd_num != 0) || (mem_wb_write.rd_is_fpr))
    && ((id_ex_write.rs1_num == mem_wb_write.rd_num && id_ex_write.rs1_is_fpr == mem_wb_write.rd_is_fpr) 
      || (id_ex_write.rs2_num == mem_wb_write.rd_num && id_ex_write.rs2_is_fpr == mem_wb_write.rd_is_fpr))){
    // stall = true;
    if(mem_wb_write.mem_to_reg){ // its a load instruction
      // in this case we need to forward memory_read_data instead of ALU_result
      load_use_hazard = true;
    }
    else{
      forward_from_mem_wb = true;
    }
  }

      

  // if(stall){
  //   // setting all control signals to zero
  //   id_ex_read.reg_write = false;
  //   id_ex_read.mem_read = false;
  //   id_ex_read.mem_write = false;
  //   id_ex_read.mem_to_reg = false;
  //   id_ex_read.alu_src = false;
  //   id_ex_read.branch = false;
  //   id_ex_read.alu_op_ = 0;
  //   id_ex_read.instruction = 0x13; // NOP instruction

  //   if_id_read.instruction = 0;

  //   // id_ex_read = {};
  //   UpdateProgramCounter(-8);
  // }
  
}


void RVSSVM::CorrectionUnit(){

  if(forward_from_mem_wb){ // forwarding alu result
    uint8_t opcode = (mem_wb_write.instruction & 0b1111111);
    if((opcode == 0b1101111 || opcode == 0b1100111)){
      jal_jalr_hazard = true;
    }
    if(id_ex_write.rs1_num == mem_wb_write.rd_num 
      && id_ex_write.rs1_is_fpr == mem_wb_write.rd_is_fpr){
      if(jal_jalr_hazard){
        id_ex_write.reg1_value = mem_wb_write.next_pc;
      }
      else{
        id_ex_write.reg1_value = mem_wb_write.alu_result;
      }
    }
    if(id_ex_write.rs2_num == mem_wb_write.rd_num 
      && id_ex_write.rs2_is_fpr == mem_wb_write.rd_is_fpr){
      if(jal_jalr_hazard){
        id_ex_write.reg2_value = mem_wb_write.next_pc;
      }
      else{
        id_ex_write.reg2_value = mem_wb_write.alu_result;
      }
    }
    jal_jalr_hazard = false;
  }
  if(load_use_hazard){ // forwarding mem_read_data
    if(id_ex_write.rs1_num == mem_wb_write.rd_num 
      && id_ex_write.rs1_is_fpr == mem_wb_write.rd_is_fpr){
      id_ex_write.reg1_value = mem_wb_write.memory_read_data;
    }
    if(id_ex_write.rs2_num == mem_wb_write.rd_num 
      && id_ex_write.rs2_is_fpr == mem_wb_write.rd_is_fpr){
      id_ex_write.reg2_value = mem_wb_write.memory_read_data;
    }
  }
  if(forward_from_ex_mem){
    uint8_t opcode = (ex_mem_write.instruction & 0b1111111);
    if((opcode == 0b1101111 || opcode == 0b1100111)){
      jal_jalr_hazard = true;
    }
    if(id_ex_write.rs1_num == ex_mem_write.rd_num 
      && id_ex_write.rs1_is_fpr == ex_mem_write.rd_is_fpr){
      if(jal_jalr_hazard){
        id_ex_write.reg1_value = ex_mem_write.next_pc;
      }
      else{
        id_ex_write.reg1_value = ex_mem_write.alu_result;
      }
    }
    if(id_ex_write.rs2_num == ex_mem_write.rd_num 
      && id_ex_write.rs2_is_fpr == ex_mem_write.rd_is_fpr){
      if(jal_jalr_hazard){
        id_ex_write.reg2_value = ex_mem_write.next_pc;
      }
      else{
        id_ex_write.reg2_value = ex_mem_write.alu_result;
      }
    }
    jal_jalr_hazard = false;
  }

  stall = false;

}



// void RVSSVM::Run() {
//   ClearStop();
//   uint64_t instruction_executed = 0;

//   // A new variable to track if the pipeline is draining
//   bool draining = false;
//   int drain_cycles = 5; // Your pipeline depth

//   // Loop condition must change
//   while (!stop_requested_) {
//     if (instruction_executed > vm_config::config.getInstructionExecutionLimit())
//       break;

//     // Check if fetch has hit the end
//     if (program_counter_ >= program_size_) {
//         draining = true;
//     }

//     WriteBack();
//     WriteMemory();
//     Execute();
//     Decode();
//     Fetch(); 
    
//     // ... Hazard/Correction units ...
    
//     if(!stall){
//       if_id_read = if_id_write;
//       id_ex_read = id_ex_write;
//     }
//     ex_mem_read = ex_mem_write;
//     mem_wb_read = mem_wb_write;

//     if(!stall){
//       instructions_retired_++;
//       instruction_executed++;
//     }
//     cycle_s_++;
//     std::cout << "Program Counter: " << program_counter_ << std::endl;

//     // If we are draining, count down cycles and then exit
//     if (draining) {
//         drain_cycles--;
//         if (drain_cycles <= 0) {
//             break; // Exit the loop
//         }
//     }
//   }

//   // ... rest of the function ...
//   // printing the register values
//   std::cout<<"--------- GPR register ---------"<<std::endl;
//   for(int i=0;i<32;i++) std::cout<<static_cast<int>(registers_.ReadGpr(i))<<" ";
//   std::cout<<std::endl;
//   std::cout<<"--------- FPR register ---------"<<std::endl;
//   for(int i=0;i<32;i++){
//     // std::cout<<static_cast<double>(registers_.ReadFpr(i))<<" ";
//     uint32_t bits = registers_.ReadFpr(i);
//     float value = *reinterpret_cast<float*>(&bits);
//     std::cout << value << " ";
//   }
//   std::cout<<std::endl;

//   if (program_counter_ >= program_size_) {
//     std::cout << "VM_PROGRAM_END" << std::endl;
//     output_status_ = "VM_PROGRAM_END";
//   }
//   DumpRegisters(globals::registers_dump_file_path, registers_);
//   DumpState(globals::vm_state_dump_file_path);
// }


void RVSSVM::Run() {
  ClearStop();
  uint64_t instruction_executed = 0;

  while (!stop_requested_ && program_counter_ < program_size_ + 8) {
    if (instruction_executed > vm_config::config.getInstructionExecutionLimit()){
      std::cout<<"Breaking Out"<<std::endl;
      break;
    }

    // reversing the execution of stages
    WriteBack();
    WriteMemory();
    Execute();
    Decode();
    Fetch();

    // std::cout<<mem_wb_read.instruction<<std::endl;

    // write a function to reset all the control signals in the orginal header to defaults, eg: bool branch_taken_ should be set to false etc


    // check for hazards and do the needful
    // HazardDetectionUnit();
    // CorrectionUnit();

    // std::cout << "Program Counter: " << mem_wb_read.pc << std::endl;
    // std::cout << "Program Counter: " << ex_mem_write.pc << std::endl;
    // std::cout << "Program Counter: " << program_counter_-12 << std::endl;

    // moving the data forward
    // if(!stall){
    //   if_id_read = if_id_write;
    //   id_ex_read = id_ex_write;
    // }
    // ex_mem_read = ex_mem_write;
    // mem_wb_read = mem_wb_write;

    if_id_read = if_id_write;
    id_ex_read = id_ex_write;
    ex_mem_read = ex_mem_write;
    mem_wb_read = mem_wb_write;

    instructions_retired_++;
    instruction_executed++;

    // if(!stall){
    //   instructions_retired_++;
    //   instruction_executed++;
    // }
    cycle_s_++;
  }

  // printing the register values
  std::cout<<"--------- GPR register ---------"<<std::endl;
  for(int i=0;i<32;i++) std::cout<<static_cast<int>(registers_.ReadGpr(i))<<" ";
  std::cout<<std::endl;
  std::cout<<"--------- FPR register ---------"<<std::endl;
  for(int i=0;i<32;i++){
    // std::cout<<static_cast<double>(registers_.ReadFpr(i))<<" ";
    uint32_t bits = registers_.ReadFpr(i);
    float value = *reinterpret_cast<float*>(&bits);
    std::cout << value << " ";
  }
  std::cout<<std::endl;

  if (program_counter_ >= program_size_) {
    std::cout << "VM_PROGRAM_END" << std::endl;
    output_status_ = "VM_PROGRAM_END";
  }
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

void RVSSVM::DebugRun() {
  ClearStop();
  uint64_t instruction_executed = 0;
  while (!stop_requested_ && program_counter_ < program_size_) {
    if (instruction_executed > vm_config::config.getInstructionExecutionLimit())
      break;
    current_delta_.old_pc = program_counter_;
    if (std::find(breakpoints_.begin(), breakpoints_.end(), program_counter_) == breakpoints_.end()) {
      Fetch();
      Decode();
      Execute();
      WriteMemory();
      WriteBack();
      instructions_retired_++;
      instruction_executed++;
      cycle_s_++;
      std::cout << "Program Counter: " << program_counter_ << std::endl;

      current_delta_.new_pc = program_counter_;
      // history_.push(current_delta_);
      undo_stack_.push(current_delta_);
      while (!redo_stack_.empty()) {
        redo_stack_.pop();
      }
      current_delta_ = StepDelta();
      if (program_counter_ < program_size_) {
        std::cout << "VM_STEP_COMPLETED" << std::endl;
        output_status_ = "VM_STEP_COMPLETED";
      } else if (program_counter_ >= program_size_) {
        std::cout << "VM_LAST_INSTRUCTION_STEPPED" << std::endl;
        output_status_ = "VM_LAST_INSTRUCTION_STEPPED";
      }
      DumpRegisters(globals::registers_dump_file_path, registers_);
      DumpState(globals::vm_state_dump_file_path);

      unsigned int delay_ms = vm_config::config.getRunStepDelay();
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      
    } else {
      std::cout << "VM_BREAKPOINT_HIT " << program_counter_ << std::endl;
      output_status_ = "VM_BREAKPOINT_HIT";
      break;
    }
  }
  if (program_counter_ >= program_size_) {
    std::cout << "VM_PROGRAM_END" << std::endl;
    output_status_ = "VM_PROGRAM_END";
  }
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

void RVSSVM::Step() {
  current_delta_.old_pc = program_counter_;
  if (program_counter_ < program_size_) {
    // run the stages in reverse ordre so that we dont 
    // write in the register data which is not been used
    hold_pc = false;
    WriteBack();
    WriteMemory();
    Execute();
    Decode();
    Fetch();

    instructions_retired_++;
    cycle_s_++;
    std::cout << "Program Counter: " << std::hex << program_counter_ << std::dec << std::endl;

    current_delta_.new_pc = program_counter_;

    // history_.push(current_delta_);

    undo_stack_.push(current_delta_);
    while (!redo_stack_.empty()) {
      redo_stack_.pop();
    }

    current_delta_ = StepDelta();


    if (program_counter_ < program_size_) {
      std::cout << "VM_STEP_COMPLETED" << std::endl;
      output_status_ = "VM_STEP_COMPLETED";
    } else if (program_counter_ >= program_size_) {
      std::cout << "VM_LAST_INSTRUCTION_STEPPED" << std::endl;
      output_status_ = "VM_LAST_INSTRUCTION_STEPPED";
    }

  } else if (program_counter_ >= program_size_) {
    std::cout << "VM_PROGRAM_END" << std::endl;
    output_status_ = "VM_PROGRAM_END";
  }
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

void RVSSVM::Undo() {
  if (undo_stack_.empty()) {
    std::cout << "VM_NO_MORE_UNDO" << std::endl;
    output_status_ = "VM_NO_MORE_UNDO";
    return;
  }

  StepDelta last = undo_stack_.top();
  undo_stack_.pop();

  // if (!history_.can_undo()) {
  //     std::cout << "Nothing to undo.\n";
  //     return;
  // }

  // StepDelta last = history_.undo();

  for (const auto &change : last.register_changes) {
    switch (change.reg_type) {
      case 0: { // GPR
        registers_.WriteGpr(change.reg_index, change.old_value);
        break;
      }
      case 1: { // CSR
        registers_.WriteCsr(change.reg_index, change.old_value);
        break;
      }
      case 2: { // FPR
        registers_.WriteFpr(change.reg_index, change.old_value);
        break;
      }
      default:std::cerr << "Invalid register type: " << change.reg_type << std::endl;
        break;
    }
  }

  for (const auto &change : last.memory_changes) {
    for (size_t i = 0; i < change.old_bytes_vec.size(); ++i) {
      memory_controller_.WriteByte(change.address + i, change.old_bytes_vec[i]);
    }
  }

  program_counter_ = last.old_pc;
  instructions_retired_--;
  cycle_s_--;
  std::cout << "Program Counter: " << program_counter_ << std::endl;

  redo_stack_.push(last);

  output_status_ = "VM_UNDO_COMPLETED";
  std::cout << "VM_UNDO_COMPLETED" << std::endl;

  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
}

void RVSSVM::Redo() {
  if (redo_stack_.empty()) {
    std::cout << "VM_NO_MORE_REDO" << std::endl;
    return;
  }

  StepDelta next = redo_stack_.top();
  redo_stack_.pop();

  // if (!history_.can_redo()) {
  //       std::cout << "Nothing to redo.\n";
  //       return;
  //   }

  //   StepDelta next = history_.redo();

  for (const auto &change : next.register_changes) {
    switch (change.reg_type) {
      case 0: { // GPR
        registers_.WriteGpr(change.reg_index, change.new_value);
        break;
      }
      case 1: { // CSR
        registers_.WriteCsr(change.reg_index, change.new_value);
        break;
      }
      case 2: { // FPR
        registers_.WriteFpr(change.reg_index, change.new_value);
        break;
      }
      default:std::cerr << "Invalid register type: " << change.reg_type << std::endl;
        break;
    }
  }

  for (const auto &change : next.memory_changes) {
    for (size_t i = 0; i < change.new_bytes_vec.size(); ++i) {
      memory_controller_.WriteByte(change.address + i, change.new_bytes_vec[i]);
    }
  }

  program_counter_ = next.new_pc;
  instructions_retired_++;
  cycle_s_++;
  DumpRegisters(globals::registers_dump_file_path, registers_);
  DumpState(globals::vm_state_dump_file_path);
  std::cout << "Program Counter: " << program_counter_ << std::endl;
  undo_stack_.push(next);

}

void RVSSVM::Reset() {
  program_counter_ = 0;
  instructions_retired_ = 0;
  cycle_s_ = 0;
  registers_.Reset();
  memory_controller_.Reset();
  control_unit_.Reset();
  branch_flag_ = false;
  next_pc_ = 0;
  execution_result_ = 0;
  memory_result_ = 0;

  return_address_ = 0;
  csr_target_address_ = 0;
  csr_old_value_ = 0;
  csr_write_val_ = 0;
  csr_uimm_ = 0;
  current_delta_.register_changes.clear();
  current_delta_.memory_changes.clear();
  current_delta_.old_pc = 0;
  current_delta_.new_pc = 0;
  undo_stack_ = std::stack<StepDelta>();
  redo_stack_ = std::stack<StepDelta>();

}