/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "arm64_lir.h"
#include "codegen_arm64.h"
#include "dex/quick/mir_to_lir-inl.h"

namespace art {

/* This file contains codegen for the A64 ISA. */

static int32_t EncodeImmSingle(uint32_t bits) {
  /*
   * Valid values will have the form:
   *
   *   aBbb.bbbc.defg.h000.0000.0000.0000.0000
   *
   * where B = not(b). In other words, if b == 1, then B == 0 and viceversa.
   */

  // bits[19..0] are cleared.
  if ((bits & 0x0007ffff) != 0)
    return -1;

  // bits[29..25] are all set or all cleared.
  uint32_t b_pattern = (bits >> 16) & 0x3e00;
  if (b_pattern != 0 && b_pattern != 0x3e00)
    return -1;

  // bit[30] and bit[29] are opposite.
  if (((bits ^ (bits << 1)) & 0x40000000) == 0)
    return -1;

  // bits: aBbb.bbbc.defg.h000.0000.0000.0000.0000
  // bit7: a000.0000
  uint32_t bit7 = ((bits >> 31) & 0x1) << 7;
  // bit6: 0b00.0000
  uint32_t bit6 = ((bits >> 29) & 0x1) << 6;
  // bit5_to_0: 00cd.efgh
  uint32_t bit5_to_0 = (bits >> 19) & 0x3f;
  return (bit7 | bit6 | bit5_to_0);
}

static int32_t EncodeImmDouble(uint64_t bits) {
  /*
   * Valid values will have the form:
   *
   *   aBbb.bbbb.bbcd.efgh.0000.0000.0000.0000
   *   0000.0000.0000.0000.0000.0000.0000.0000
   *
   * where B = not(b).
   */

  // bits[47..0] are cleared.
  if ((bits & UINT64_C(0xffffffffffff)) != 0)
    return -1;

  // bits[61..54] are all set or all cleared.
  uint32_t b_pattern = (bits >> 48) & 0x3fc0;
  if (b_pattern != 0 && b_pattern != 0x3fc0)
    return -1;

  // bit[62] and bit[61] are opposite.
  if (((bits ^ (bits << 1)) & UINT64_C(0x4000000000000000)) == 0)
    return -1;

  // bit7: a000.0000
  uint32_t bit7 = ((bits >> 63) & 0x1) << 7;
  // bit6: 0b00.0000
  uint32_t bit6 = ((bits >> 61) & 0x1) << 6;
  // bit5_to_0: 00cd.efgh
  uint32_t bit5_to_0 = (bits >> 48) & 0x3f;
  return (bit7 | bit6 | bit5_to_0);
}

LIR* Arm64Mir2Lir::LoadFPConstantValue(int r_dest, int32_t value) {
  DCHECK(RegStorage::IsSingle(r_dest));
  if (value == 0) {
    return NewLIR2(kA64Fmov2sw, r_dest, rwzr);
  } else {
    int32_t encoded_imm = EncodeImmSingle((uint32_t)value);
    if (encoded_imm >= 0) {
      return NewLIR2(kA64Fmov2fI, r_dest, encoded_imm);
    }
  }

  LIR* data_target = ScanLiteralPool(literal_list_, value, 0);
  if (data_target == NULL) {
    data_target = AddWordData(&literal_list_, value);
  }

  LIR* load_pc_rel = RawLIR(current_dalvik_offset_, kA64Ldr2fp,
                            r_dest, 0, 0, 0, 0, data_target);
  SetMemRefType(load_pc_rel, true, kLiteral);
  AppendLIR(load_pc_rel);
  return load_pc_rel;
}

LIR* Arm64Mir2Lir::LoadFPConstantValueWide(int r_dest, int64_t value) {
  DCHECK(RegStorage::IsDouble(r_dest));
  if (value == 0) {
    return NewLIR2(kA64Fmov2Sx, r_dest, rwzr);
  } else {
    int32_t encoded_imm = EncodeImmDouble(value);
    if (encoded_imm >= 0) {
      return NewLIR2(FWIDE(kA64Fmov2fI), r_dest, encoded_imm);
    }
  }

  // No short form - load from the literal pool.
  int32_t val_lo = Low32Bits(value);
  int32_t val_hi = High32Bits(value);
  LIR* data_target = ScanLiteralPoolWide(literal_list_, val_lo, val_hi);
  if (data_target == NULL) {
    data_target = AddWideData(&literal_list_, val_lo, val_hi);
  }

  DCHECK(RegStorage::IsFloat(r_dest));
  LIR* load_pc_rel = RawLIR(current_dalvik_offset_, FWIDE(kA64Ldr2fp),
                            r_dest, 0, 0, 0, 0, data_target);
  SetMemRefType(load_pc_rel, true, kLiteral);
  AppendLIR(load_pc_rel);
  return load_pc_rel;
}

static int CountLeadingZeros(bool is_wide, uint64_t value) {
  return (is_wide) ? __builtin_clzl(value) : __builtin_clz((uint32_t)value);
}

static int CountTrailingZeros(bool is_wide, uint64_t value) {
  return (is_wide) ? __builtin_ctzl(value) : __builtin_ctz((uint32_t)value);
}

static int CountSetBits(bool is_wide, uint64_t value) {
  return ((is_wide) ?
          __builtin_popcountl(value) : __builtin_popcount((uint32_t)value));
}

/**
 * @brief Try encoding an immediate in the form required by logical instructions.
 *
 * @param is_wide Whether @p value is a 64-bit (as opposed to 32-bit) value.
 * @param value An integer to be encoded. This is interpreted as 64-bit if @p is_wide is true and as
 *   32-bit if @p is_wide is false.
 * @return A non-negative integer containing the encoded immediate or -1 if the encoding failed.
 * @note This is the inverse of Arm64Mir2Lir::DecodeLogicalImmediate().
 */
int Arm64Mir2Lir::EncodeLogicalImmediate(bool is_wide, uint64_t value) {
  unsigned n, imm_s, imm_r;

  // Logical immediates are encoded using parameters n, imm_s and imm_r using
  // the following table:
  //
  //  N   imms    immr    size        S             R
  //  1  ssssss  rrrrrr    64    UInt(ssssss)  UInt(rrrrrr)
  //  0  0sssss  xrrrrr    32    UInt(sssss)   UInt(rrrrr)
  //  0  10ssss  xxrrrr    16    UInt(ssss)    UInt(rrrr)
  //  0  110sss  xxxrrr     8    UInt(sss)     UInt(rrr)
  //  0  1110ss  xxxxrr     4    UInt(ss)      UInt(rr)
  //  0  11110s  xxxxxr     2    UInt(s)       UInt(r)
  // (s bits must not be all set)
  //
  // A pattern is constructed of size bits, where the least significant S+1
  // bits are set. The pattern is rotated right by R, and repeated across a
  // 32 or 64-bit value, depending on destination register width.
  //
  // To test if an arbitary immediate can be encoded using this scheme, an
  // iterative algorithm is used.
  //

  // 1. If the value has all set or all clear bits, it can't be encoded.
  if (value == 0 || value == ~UINT64_C(0) ||
      (!is_wide && (uint32_t)value == ~UINT32_C(0))) {
    return -1;
  }

  unsigned lead_zero  = CountLeadingZeros(is_wide, value);
  unsigned lead_one   = CountLeadingZeros(is_wide, ~value);
  unsigned trail_zero = CountTrailingZeros(is_wide, value);
  unsigned trail_one  = CountTrailingZeros(is_wide, ~value);
  unsigned set_bits   = CountSetBits(is_wide, value);

  // The fixed bits in the immediate s field.
  // If width == 64 (X reg), start at 0xFFFFFF80.
  // If width == 32 (W reg), start at 0xFFFFFFC0, as the iteration for 64-bit
  // widths won't be executed.
  unsigned width = (is_wide) ? 64 : 32;
  int imm_s_fixed = (is_wide) ? -128 : -64;
  int imm_s_mask = 0x3f;

  for (;;) {
    // 2. If the value is two bits wide, it can be encoded.
    if (width == 2) {
      n = 0;
      imm_s = 0x3C;
      imm_r = (value & 3) - 1;
      break;
    }

    n = (width == 64) ? 1 : 0;
    imm_s = ((imm_s_fixed | (set_bits - 1)) & imm_s_mask);
    if ((lead_zero + set_bits) == width) {
      imm_r = 0;
    } else {
      imm_r = (lead_zero > 0) ? (width - trail_zero) : lead_one;
    }

    // 3. If the sum of leading zeros, trailing zeros and set bits is
    //    equal to the bit width of the value, it can be encoded.
    if (lead_zero + trail_zero + set_bits == width) {
      break;
    }

    // 4. If the sum of leading ones, trailing ones and unset bits in the
    //    value is equal to the bit width of the value, it can be encoded.
    if (lead_one + trail_one + (width - set_bits) == width) {
      break;
    }

    // 5. If the most-significant half of the bitwise value is equal to
    //    the least-significant half, return to step 2 using the
    //    least-significant half of the value.
    uint64_t mask = (UINT64_C(1) << (width >> 1)) - 1;
    if ((value & mask) == ((value >> (width >> 1)) & mask)) {
      width >>= 1;
      set_bits >>= 1;
      imm_s_fixed >>= 1;
      continue;
    }

    // 6. Otherwise, the value can't be encoded.
    return -1;
  }

  return (n << 12 | imm_r << 6 | imm_s);
}

bool Arm64Mir2Lir::InexpensiveConstantInt(int32_t value) {
  return false;  // (ModifiedImmediate(value) >= 0) || (ModifiedImmediate(~value) >= 0);
}

bool Arm64Mir2Lir::InexpensiveConstantFloat(int32_t value) {
  return EncodeImmSingle(value) >= 0;
}

bool Arm64Mir2Lir::InexpensiveConstantLong(int64_t value) {
  return InexpensiveConstantInt(High32Bits(value)) && InexpensiveConstantInt(Low32Bits(value));
}

bool Arm64Mir2Lir::InexpensiveConstantDouble(int64_t value) {
  return EncodeImmDouble(value) >= 0;
}

/*
 * Load a immediate using one single instruction when possible; otherwise
 * use a pair of movz and movk instructions.
 *
 * No additional register clobbering operation performed. Use this version when
 * 1) r_dest is freshly returned from AllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR* Arm64Mir2Lir::LoadConstantNoClobber(RegStorage r_dest, int value) {
  LIR* res;

  if (r_dest.IsFloat()) {
    return LoadFPConstantValue(r_dest.GetReg(), value);
  }

  // Loading SP/ZR with an immediate is not supported.
  DCHECK_NE(r_dest.GetReg(), rwsp);
  DCHECK_NE(r_dest.GetReg(), rwzr);

  // Compute how many movk, movz instructions are needed to load the value.
  uint16_t high_bits = High16Bits(value);
  uint16_t low_bits = Low16Bits(value);

  bool low_fast = ((uint16_t)(low_bits + 1) <= 1);
  bool high_fast = ((uint16_t)(high_bits + 1) <= 1);

  if (LIKELY(low_fast || high_fast)) {
    // 1 instruction is enough to load the immediate.
    if (LIKELY(low_bits == high_bits)) {
      // Value is either 0 or -1: we can just use wzr.
      ArmOpcode opcode = LIKELY(low_bits == 0) ? kA64Mov2rr : kA64Mvn2rr;
      res = NewLIR2(opcode, r_dest.GetReg(), rwzr);
    } else {
      uint16_t uniform_bits, useful_bits;
      int shift;

      if (LIKELY(high_fast)) {
        shift = 0;
        uniform_bits = high_bits;
        useful_bits = low_bits;
      } else {
        shift = 1;
        uniform_bits = low_bits;
        useful_bits = high_bits;
      }

      if (UNLIKELY(uniform_bits != 0)) {
        res = NewLIR3(kA64Movn3rdM, r_dest.GetReg(), ~useful_bits, shift);
      } else {
        res = NewLIR3(kA64Movz3rdM, r_dest.GetReg(), useful_bits, shift);
      }
    }
  } else {
    // movk, movz require 2 instructions. Try detecting logical immediates.
    int log_imm = EncodeLogicalImmediate(/*is_wide=*/false, value);
    if (log_imm >= 0) {
      res = NewLIR3(kA64Orr3Rrl, r_dest.GetReg(), rwzr, log_imm);
    } else {
      // Use 2 instructions.
      res = NewLIR3(kA64Movz3rdM, r_dest.GetReg(), low_bits, 0);
      NewLIR3(kA64Movk3rdM, r_dest.GetReg(), high_bits, 1);
    }
  }

  return res;
}

LIR* Arm64Mir2Lir::OpUnconditionalBranch(LIR* target) {
  LIR* res = NewLIR1(kA64B1t, 0 /* offset to be patched  during assembly */);
  res->target = target;
  return res;
}

LIR* Arm64Mir2Lir::OpCondBranch(ConditionCode cc, LIR* target) {
  LIR* branch = NewLIR2(kA64B2ct, ArmConditionEncoding(cc),
                        0 /* offset to be patched */);
  branch->target = target;
  return branch;
}

LIR* Arm64Mir2Lir::OpReg(OpKind op, RegStorage r_dest_src) {
  ArmOpcode opcode = kA64Brk1d;
  switch (op) {
    case kOpBlx:
      opcode = kA64Blr1x;
      break;
    // TODO(Arm64): port kThumbBx.
    // case kOpBx:
    //   opcode = kThumbBx;
    //   break;
    default:
      LOG(FATAL) << "Bad opcode " << op;
  }
  return NewLIR1(opcode, r_dest_src.GetReg());
}

LIR* Arm64Mir2Lir::OpRegRegShift(OpKind op, RegStorage r_dest_src1, RegStorage r_src2, int shift) {
  ArmOpcode wide = (r_dest_src1.Is64Bit()) ? WIDE(0) : UNWIDE(0);
  CHECK_EQ(r_dest_src1.Is64Bit(), r_src2.Is64Bit());
  ArmOpcode opcode = kA64Brk1d;

  switch (op) {
    case kOpCmn:
      opcode = kA64Cmn3rro;
      break;
    case kOpCmp:
      opcode = kA64Cmp3rro;
      break;
    case kOpMov:
      opcode = kA64Mov2rr;
      break;
    case kOpMvn:
      opcode = kA64Mvn2rr;
      break;
    case kOpNeg:
      opcode = kA64Neg3rro;
      break;
    case kOpTst:
      opcode = kA64Tst3rro;
      break;
    case kOpRev:
      DCHECK_EQ(shift, 0);
      // Binary, but rm is encoded twice.
      return NewLIR3(kA64Rev2rr | wide, r_dest_src1.GetReg(), r_src2.GetReg(), r_src2.GetReg());
      break;
    case kOpRevsh:
      // Binary, but rm is encoded twice.
      return NewLIR3(kA64Rev162rr | wide, r_dest_src1.GetReg(), r_src2.GetReg(), r_src2.GetReg());
      break;
    case kOp2Byte:
      DCHECK_EQ(shift, ENCODE_NO_SHIFT);
      // "sbfx r1, r2, #imm1, #imm2" is "sbfm r1, r2, #imm1, #(imm1 + imm2 - 1)".
      // For now we use sbfm directly.
      return NewLIR4(kA64Sbfm4rrdd | wide, r_dest_src1.GetReg(), r_src2.GetReg(), 0, 7);
    case kOp2Short:
      DCHECK_EQ(shift, ENCODE_NO_SHIFT);
      // For now we use sbfm rather than its alias, sbfx.
      return NewLIR4(kA64Sbfm4rrdd | wide, r_dest_src1.GetReg(), r_src2.GetReg(), 0, 15);
    case kOp2Char:
      // "ubfx r1, r2, #imm1, #imm2" is "ubfm r1, r2, #imm1, #(imm1 + imm2 - 1)".
      // For now we use ubfm directly.
      DCHECK_EQ(shift, ENCODE_NO_SHIFT);
      return NewLIR4(kA64Ubfm4rrdd | wide, r_dest_src1.GetReg(), r_src2.GetReg(), 0, 15);
    default:
      return OpRegRegRegShift(op, r_dest_src1, r_dest_src1, r_src2, shift);
  }

  DCHECK(!IsPseudoLirOp(opcode));
  if (EncodingMap[opcode].flags & IS_BINARY_OP) {
    DCHECK_EQ(shift, ENCODE_NO_SHIFT);
    return NewLIR2(opcode | wide, r_dest_src1.GetReg(), r_src2.GetReg());
  } else if (EncodingMap[opcode].flags & IS_TERTIARY_OP) {
    ArmEncodingKind kind = EncodingMap[opcode].field_loc[2].kind;
    if (kind == kFmtShift) {
      return NewLIR3(opcode | wide, r_dest_src1.GetReg(), r_src2.GetReg(), shift);
    }
  }

  LOG(FATAL) << "Unexpected encoding operand count";
  return NULL;
}

LIR* Arm64Mir2Lir::OpRegReg(OpKind op, RegStorage r_dest_src1, RegStorage r_src2) {
  return OpRegRegShift(op, r_dest_src1, r_src2, ENCODE_NO_SHIFT);
}

LIR* Arm64Mir2Lir::OpMovRegMem(RegStorage r_dest, RegStorage r_base, int offset, MoveType move_type) {
  UNIMPLEMENTED(FATAL);
  return nullptr;
}

LIR* Arm64Mir2Lir::OpMovMemReg(RegStorage r_base, int offset, RegStorage r_src, MoveType move_type) {
  UNIMPLEMENTED(FATAL);
  return nullptr;
}

LIR* Arm64Mir2Lir::OpCondRegReg(OpKind op, ConditionCode cc, RegStorage r_dest, RegStorage r_src) {
  LOG(FATAL) << "Unexpected use of OpCondRegReg for Arm64";
  return NULL;
}

LIR* Arm64Mir2Lir::OpRegRegRegShift(OpKind op, RegStorage r_dest, RegStorage r_src1,
                                    RegStorage r_src2, int shift) {
  ArmOpcode opcode = kA64Brk1d;

  switch (op) {
    case kOpAdd:
      opcode = kA64Add4rrro;
      break;
    case kOpSub:
      opcode = kA64Sub4rrro;
      break;
    // case kOpRsub:
    //   opcode = kA64RsubWWW;
    //   break;
    case kOpAdc:
      opcode = kA64Adc3rrr;
      break;
    case kOpAnd:
      opcode = kA64And4rrro;
      break;
    case kOpXor:
      opcode = kA64Eor4rrro;
      break;
    case kOpMul:
      opcode = kA64Mul3rrr;
      break;
    case kOpDiv:
      opcode = kA64Sdiv3rrr;
      break;
    case kOpOr:
      opcode = kA64Orr4rrro;
      break;
    case kOpSbc:
      opcode = kA64Sbc3rrr;
      break;
    case kOpLsl:
      opcode = kA64Lsl3rrr;
      break;
    case kOpLsr:
      opcode = kA64Lsr3rrr;
      break;
    case kOpAsr:
      opcode = kA64Asr3rrr;
      break;
    case kOpRor:
      opcode = kA64Ror3rrr;
      break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }

  // The instructions above belong to two kinds:
  // - 4-operands instructions, where the last operand is a shift/extend immediate,
  // - 3-operands instructions with no shift/extend.
  ArmOpcode widened_opcode = r_dest.Is64Bit() ? WIDE(opcode) : opcode;
  CHECK_EQ(r_dest.Is64Bit(), r_src1.Is64Bit());
  CHECK_EQ(r_dest.Is64Bit(), r_src2.Is64Bit());
  if (EncodingMap[opcode].flags & IS_QUAD_OP) {
    DCHECK(!IsExtendEncoding(shift));
    return NewLIR4(widened_opcode, r_dest.GetReg(), r_src1.GetReg(), r_src2.GetReg(), shift);
  } else {
    DCHECK(EncodingMap[opcode].flags & IS_TERTIARY_OP);
    DCHECK_EQ(shift, ENCODE_NO_SHIFT);
    return NewLIR3(widened_opcode, r_dest.GetReg(), r_src1.GetReg(), r_src2.GetReg());
  }
}

LIR* Arm64Mir2Lir::OpRegRegReg(OpKind op, RegStorage r_dest, RegStorage r_src1, RegStorage r_src2) {
  return OpRegRegRegShift(op, r_dest, r_src1, r_src2, ENCODE_NO_SHIFT);
}

// Should be taking an int64_t value ?
LIR* Arm64Mir2Lir::OpRegRegImm(OpKind op, RegStorage r_dest, RegStorage r_src1, int value) {
  LIR* res;
  bool neg = (value < 0);
  int64_t abs_value = (neg) ? -value : value;
  ArmOpcode opcode = kA64Brk1d;
  ArmOpcode alt_opcode = kA64Brk1d;
  int32_t log_imm = -1;
  bool is_wide = r_dest.Is64Bit();
  CHECK_EQ(r_dest.Is64Bit(), r_src1.Is64Bit());
  ArmOpcode wide = (is_wide) ? WIDE(0) : UNWIDE(0);

  switch (op) {
    case kOpLsl: {
      // "lsl w1, w2, #imm" is an alias of "ubfm w1, w2, #(-imm MOD 32), #(31-imm)"
      // and "lsl x1, x2, #imm" of "ubfm x1, x2, #(-imm MOD 32), #(31-imm)".
      // For now, we just use ubfm directly.
      int max_value = (is_wide) ? 64 : 32;
      return NewLIR4(kA64Ubfm4rrdd | wide, r_dest.GetReg(), r_src1.GetReg(),
                     (-value) & (max_value - 1), max_value - value);
    }
    case kOpLsr:
      return NewLIR3(kA64Lsr3rrd | wide, r_dest.GetReg(), r_src1.GetReg(), value);
    case kOpAsr:
      return NewLIR3(kA64Asr3rrd | wide, r_dest.GetReg(), r_src1.GetReg(), value);
    case kOpRor:
      // "ror r1, r2, #imm" is an alias of "extr r1, r2, r2, #imm".
      // For now, we just use extr directly.
      return NewLIR4(kA64Extr4rrrd | wide, r_dest.GetReg(), r_src1.GetReg(), r_src1.GetReg(),
                     value);
    case kOpAdd:
      neg = !neg;
      // Note: intentional fallthrough
    case kOpSub:
      // Add and sub below read/write sp rather than xzr.
      if (abs_value < 0x1000) {
        opcode = (neg) ? kA64Add4RRdT : kA64Sub4RRdT;
        return NewLIR4(opcode | wide, r_dest.GetReg(), r_src1.GetReg(), abs_value, 0);
      } else if ((abs_value & UINT64_C(0xfff)) == 0 && ((abs_value >> 12) < 0x1000)) {
        opcode = (neg) ? kA64Add4RRdT : kA64Sub4RRdT;
        return NewLIR4(opcode | wide, r_dest.GetReg(), r_src1.GetReg(), abs_value >> 12, 1);
      } else {
        log_imm = -1;
        alt_opcode = (neg) ? kA64Add4rrro : kA64Sub4rrro;
      }
      break;
    // case kOpRsub:
    //   opcode = kThumb2RsubRRI8M;
    //   alt_opcode = kThumb2RsubRRR;
    //   break;
    case kOpAdc:
      log_imm = -1;
      alt_opcode = kA64Adc3rrr;
      break;
    case kOpSbc:
      log_imm = -1;
      alt_opcode = kA64Sbc3rrr;
      break;
    case kOpOr:
      log_imm = EncodeLogicalImmediate(is_wide, value);
      opcode = kA64Orr3Rrl;
      alt_opcode = kA64Orr4rrro;
      break;
    case kOpAnd:
      log_imm = EncodeLogicalImmediate(is_wide, value);
      opcode = kA64And3Rrl;
      alt_opcode = kA64And4rrro;
      break;
    case kOpXor:
      log_imm = EncodeLogicalImmediate(is_wide, value);
      opcode = kA64Eor3Rrl;
      alt_opcode = kA64Eor4rrro;
      break;
    case kOpMul:
      // TUNING: power of 2, shift & add
      log_imm = -1;
      alt_opcode = kA64Mul3rrr;
      break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
  }

  if (log_imm >= 0) {
    return NewLIR3(opcode | wide, r_dest.GetReg(), r_src1.GetReg(), log_imm);
  } else {
    RegStorage r_scratch = AllocTemp();
    LoadConstant(r_scratch, value);
    if (EncodingMap[alt_opcode].flags & IS_QUAD_OP)
      res = NewLIR4(alt_opcode, r_dest.GetReg(), r_src1.GetReg(), r_scratch.GetReg(), 0);
    else
      res = NewLIR3(alt_opcode, r_dest.GetReg(), r_src1.GetReg(), r_scratch.GetReg());
    FreeTemp(r_scratch);
    return res;
  }
}

LIR* Arm64Mir2Lir::OpRegImm(OpKind op, RegStorage r_dest_src1, int value) {
  return OpRegImm64(op, r_dest_src1, static_cast<int64_t>(value));
}

LIR* Arm64Mir2Lir::OpRegImm64(OpKind op, RegStorage r_dest_src1, int64_t value) {
  ArmOpcode wide = (r_dest_src1.Is64Bit()) ? WIDE(0) : UNWIDE(0);
  ArmOpcode opcode = kA64Brk1d;
  ArmOpcode neg_opcode = kA64Brk1d;
  bool shift;
  bool neg = (value < 0);
  uint64_t abs_value = (neg) ? -value : value;

  if (LIKELY(abs_value < 0x1000)) {
    // abs_value is a 12-bit immediate.
    shift = false;
  } else if ((abs_value & UINT64_C(0xfff)) == 0 && ((abs_value >> 12) < 0x1000)) {
    // abs_value is a shifted 12-bit immediate.
    shift = true;
    abs_value >>= 12;
  } else {
    RegStorage r_tmp = AllocTemp();
    LIR* res = LoadConstant(r_tmp, value);
    OpRegReg(op, r_dest_src1, r_tmp);
    FreeTemp(r_tmp);
    return res;
  }

  switch (op) {
    case kOpAdd:
      neg_opcode = kA64Sub4RRdT;
      opcode = kA64Add4RRdT;
      break;
    case kOpSub:
      neg_opcode = kA64Add4RRdT;
      opcode = kA64Sub4RRdT;
      break;
    case kOpCmp:
      neg_opcode = kA64Cmn3RdT;
      opcode = kA64Cmp3RdT;
      break;
    default:
      LOG(FATAL) << "Bad op-kind in OpRegImm: " << op;
      break;
  }

  if (UNLIKELY(neg))
    opcode = neg_opcode;

  if (EncodingMap[opcode].flags & IS_QUAD_OP)
    return NewLIR4(opcode | wide, r_dest_src1.GetReg(), r_dest_src1.GetReg(), abs_value,
                   (shift) ? 1 : 0);
  else
    return NewLIR3(opcode | wide, r_dest_src1.GetReg(), abs_value, (shift) ? 1 : 0);
}

LIR* Arm64Mir2Lir::LoadConstantWide(RegStorage r_dest, int64_t value) {
  if (r_dest.IsFloat()) {
    return LoadFPConstantValueWide(r_dest.GetReg(), value);
  } else {
    // TODO(Arm64): check whether we can load the immediate with a short form.
    //   e.g. via movz, movk or via logical immediate.

    // No short form - load from the literal pool.
    int32_t val_lo = Low32Bits(value);
    int32_t val_hi = High32Bits(value);
    LIR* data_target = ScanLiteralPoolWide(literal_list_, val_lo, val_hi);
    if (data_target == NULL) {
      data_target = AddWideData(&literal_list_, val_lo, val_hi);
    }

    LIR* res = RawLIR(current_dalvik_offset_, WIDE(kA64Ldr2rp),
                      r_dest.GetReg(), 0, 0, 0, 0, data_target);
    SetMemRefType(res, true, kLiteral);
    AppendLIR(res);
    return res;
  }
}

int Arm64Mir2Lir::EncodeShift(int shift_type, int amount) {
  return ((shift_type & 0x3) << 7) | (amount & 0x1f);
}

int Arm64Mir2Lir::EncodeExtend(int extend_type, int amount) {
  return  (1 << 6) | ((extend_type & 0x7) << 3) | (amount & 0x7);
}

bool Arm64Mir2Lir::IsExtendEncoding(int encoded_value) {
  return ((1 << 6) & encoded_value) != 0;
}

LIR* Arm64Mir2Lir::LoadBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_dest,
                                   int scale, OpSize size) {
  LIR* load;
  int expected_scale = 0;
  ArmOpcode opcode = kA64Brk1d;

  if (r_dest.IsFloat()) {
    if (r_dest.IsDouble()) {
      DCHECK(size == k64 || size == kDouble);
      expected_scale = 3;
      opcode = FWIDE(kA64Ldr4fXxG);
    } else {
      DCHECK(r_dest.IsSingle());
      DCHECK(size == k32 || size == kSingle);
      expected_scale = 2;
      opcode = kA64Ldr4fXxG;
    }

    DCHECK(scale == 0 || scale == expected_scale);
    return NewLIR4(opcode, r_dest.GetReg(), r_base.GetReg(), r_index.GetReg(),
                   (scale != 0) ? 1 : 0);
  }

  switch (size) {
    case kDouble:
    case kWord:
    case k64:
      opcode = WIDE(kA64Ldr4rXxG);
      expected_scale = 3;
      break;
    case kSingle:
    case k32:
    case kReference:
      opcode = kA64Ldr4rXxG;
      expected_scale = 2;
      break;
    case kUnsignedHalf:
      opcode = kA64Ldrh4wXxd;
      expected_scale = 1;
      break;
    case kSignedHalf:
      opcode = kA64Ldrsh4rXxd;
      expected_scale = 1;
      break;
    case kUnsignedByte:
      opcode = kA64Ldrb3wXx;
      break;
    case kSignedByte:
      opcode = kA64Ldrsb3rXx;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }

  if (UNLIKELY(expected_scale == 0)) {
    // This is a tertiary op (e.g. ldrb, ldrsb), it does not not support scale.
    DCHECK_NE(EncodingMap[UNWIDE(opcode)].flags & IS_TERTIARY_OP, 0U);
    DCHECK_EQ(scale, 0);
    load = NewLIR3(opcode, r_dest.GetReg(), r_base.GetReg(), r_index.GetReg());
  } else {
    DCHECK(scale == 0 || scale == expected_scale);
    load = NewLIR4(opcode, r_dest.GetReg(), r_base.GetReg(), r_index.GetReg(),
                   (scale != 0) ? 1 : 0);
  }

  return load;
}

LIR* Arm64Mir2Lir::StoreBaseIndexed(RegStorage r_base, RegStorage r_index, RegStorage r_src,
                                    int scale, OpSize size) {
  LIR* store;
  int expected_scale = 0;
  ArmOpcode opcode = kA64Brk1d;

  if (r_src.IsFloat()) {
    if (r_src.IsDouble()) {
      DCHECK(size == k64 || size == kDouble);
      expected_scale = 3;
      opcode = FWIDE(kA64Str4fXxG);
    } else {
      DCHECK(r_src.IsSingle());
      DCHECK(size == k32 || size == kSingle);
      expected_scale = 2;
      opcode = kA64Str4fXxG;
    }

    DCHECK(scale == 0 || scale == expected_scale);
    return NewLIR4(opcode, r_src.GetReg(), r_base.GetReg(), r_index.GetReg(),
                   (scale != 0) ? 1 : 0);
  }

  switch (size) {
    case kDouble:     // Intentional fall-trough.
    case kWord:       // Intentional fall-trough.
    case k64:
      opcode = WIDE(kA64Str4rXxG);
      expected_scale = 3;
      break;
    case kSingle:     // Intentional fall-trough.
    case k32:         // Intentional fall-trough.
    case kReference:
      opcode = kA64Str4rXxG;
      expected_scale = 2;
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = kA64Strh4wXxd;
      expected_scale = 1;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kA64Strb3wXx;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }

  if (UNLIKELY(expected_scale == 0)) {
    // This is a tertiary op (e.g. strb), it does not not support scale.
    DCHECK_NE(EncodingMap[UNWIDE(opcode)].flags & IS_TERTIARY_OP, 0U);
    DCHECK_EQ(scale, 0);
    store = NewLIR3(opcode, r_src.GetReg(), r_base.GetReg(), r_index.GetReg());
  } else {
    store = NewLIR4(opcode, r_src.GetReg(), r_base.GetReg(), r_index.GetReg(),
                    (scale != 0) ? 1 : 0);
  }

  return store;
}

/*
 * Load value from base + displacement.  Optionally perform null check
 * on base (which must have an associated s_reg and MIR).  If not
 * performing null check, incoming MIR can be null.
 */
LIR* Arm64Mir2Lir::LoadBaseDispBody(RegStorage r_base, int displacement, RegStorage r_dest,
                                    OpSize size) {
  LIR* load = NULL;
  ArmOpcode opcode = kA64Brk1d;
  ArmOpcode alt_opcode = kA64Brk1d;
  int scale = 0;

  switch (size) {
    case kDouble:     // Intentional fall-through.
    case kWord:       // Intentional fall-through.
    case k64:
      scale = 3;
      if (r_dest.IsFloat()) {
        DCHECK(r_dest.IsDouble());
        opcode = FWIDE(kA64Ldr3fXD);
        alt_opcode = FWIDE(kA64Ldur3fXd);
      } else {
        opcode = WIDE(kA64Ldr3rXD);
        alt_opcode = WIDE(kA64Ldur3rXd);
      }
      break;
    case kSingle:     // Intentional fall-through.
    case k32:         // Intentional fall-trough.
    case kReference:
      scale = 2;
      if (r_dest.IsFloat()) {
        DCHECK(r_dest.IsSingle());
        opcode = kA64Ldr3fXD;
      } else {
        opcode = kA64Ldr3rXD;
      }
      break;
    case kUnsignedHalf:
      scale = 1;
      opcode = kA64Ldrh3wXF;
      break;
    case kSignedHalf:
      scale = 1;
      opcode = kA64Ldrsh3rXF;
      break;
    case kUnsignedByte:
      opcode = kA64Ldrb3wXd;
      break;
    case kSignedByte:
      opcode = kA64Ldrsb3rXd;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }

  bool displacement_is_aligned = (displacement & ((1 << scale) - 1)) == 0;
  int scaled_disp = displacement >> scale;
  if (displacement_is_aligned && scaled_disp >= 0 && scaled_disp < 4096) {
    // Can use scaled load.
    load = NewLIR3(opcode, r_dest.GetReg(), r_base.GetReg(), scaled_disp);
  } else if (alt_opcode != kA64Brk1d && IS_SIGNED_IMM9(displacement)) {
    // Can use unscaled load.
    load = NewLIR3(alt_opcode, r_dest.GetReg(), r_base.GetReg(), displacement);
  } else {
    // Use long sequence.
    RegStorage r_scratch = AllocTemp();
    LoadConstant(r_scratch, displacement);
    load = LoadBaseIndexed(r_base, r_scratch, r_dest, 0, size);
    FreeTemp(r_scratch);
  }

  // TODO: in future may need to differentiate Dalvik accesses w/ spills
  if (r_base == rs_rA64_SP) {
    AnnotateDalvikRegAccess(load, displacement >> 2, true /* is_load */, r_dest.Is64Bit());
  }
  return load;
}

LIR* Arm64Mir2Lir::LoadBaseDispVolatile(RegStorage r_base, int displacement, RegStorage r_dest,
                                        OpSize size) {
  // LoadBaseDisp() will emit correct insn for atomic load on arm64
  // assuming r_dest is correctly prepared using RegClassForFieldLoadStore().
  return LoadBaseDisp(r_base, displacement, r_dest, size);
}

LIR* Arm64Mir2Lir::LoadBaseDisp(RegStorage r_base, int displacement, RegStorage r_dest,
                                OpSize size) {
  return LoadBaseDispBody(r_base, displacement, r_dest, size);
}


LIR* Arm64Mir2Lir::StoreBaseDispBody(RegStorage r_base, int displacement, RegStorage r_src,
                                     OpSize size) {
  LIR* store = NULL;
  ArmOpcode opcode = kA64Brk1d;
  ArmOpcode alt_opcode = kA64Brk1d;
  int scale = 0;

  switch (size) {
    case kDouble:     // Intentional fall-through.
    case kWord:       // Intentional fall-through.
    case k64:
      scale = 3;
      if (r_src.IsFloat()) {
        DCHECK(r_src.IsDouble());
        opcode = FWIDE(kA64Str3fXD);
        alt_opcode = FWIDE(kA64Stur3fXd);
      } else {
        opcode = FWIDE(kA64Str3rXD);
        alt_opcode = FWIDE(kA64Stur3rXd);
      }
      break;
    case kSingle:     // Intentional fall-through.
    case k32:         // Intentional fall-trough.
    case kReference:
      scale = 2;
      if (r_src.IsFloat()) {
        DCHECK(r_src.IsSingle());
        opcode = kA64Str3fXD;
      } else {
        opcode = kA64Str3rXD;
      }
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      scale = 1;
      opcode = kA64Strh3wXF;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = kA64Strb3wXd;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }

  bool displacement_is_aligned = (displacement & ((1 << scale) - 1)) == 0;
  int scaled_disp = displacement >> scale;
  if (displacement_is_aligned && scaled_disp >= 0 && scaled_disp < 4096) {
    // Can use scaled store.
    store = NewLIR3(opcode, r_src.GetReg(), r_base.GetReg(), scaled_disp);
  } else if (alt_opcode != kA64Brk1d && IS_SIGNED_IMM9(displacement)) {
    // Can use unscaled store.
    store = NewLIR3(alt_opcode, r_src.GetReg(), r_base.GetReg(), displacement);
  } else {
    // Use long sequence.
    RegStorage r_scratch = AllocTemp();
    LoadConstant(r_scratch, displacement);
    store = StoreBaseIndexed(r_base, r_scratch, r_src, 0, size);
    FreeTemp(r_scratch);
  }

  // TODO: In future, may need to differentiate Dalvik & spill accesses.
  if (r_base == rs_rA64_SP) {
    AnnotateDalvikRegAccess(store, displacement >> 2, false /* is_load */, r_src.Is64Bit());
  }
  return store;
}

LIR* Arm64Mir2Lir::StoreBaseDispVolatile(RegStorage r_base, int displacement, RegStorage r_src,
                                         OpSize size) {
  // StoreBaseDisp() will emit correct insn for atomic store on arm64
  // assuming r_dest is correctly prepared using RegClassForFieldLoadStore().
  return StoreBaseDisp(r_base, displacement, r_src, size);
}

LIR* Arm64Mir2Lir::StoreBaseDisp(RegStorage r_base, int displacement, RegStorage r_src,
                                 OpSize size) {
  return StoreBaseDispBody(r_base, displacement, r_src, size);
}

LIR* Arm64Mir2Lir::OpFpRegCopy(RegStorage r_dest, RegStorage r_src) {
  LOG(FATAL) << "Unexpected use of OpFpRegCopy for Arm64";
  return NULL;
}

LIR* Arm64Mir2Lir::OpThreadMem(OpKind op, ThreadOffset<4> thread_offset) {
  UNIMPLEMENTED(FATAL) << "Should not be used.";
  return nullptr;
}

LIR* Arm64Mir2Lir::OpThreadMem(OpKind op, ThreadOffset<8> thread_offset) {
  LOG(FATAL) << "Unexpected use of OpThreadMem for Arm64";
  return NULL;
}

LIR* Arm64Mir2Lir::OpMem(OpKind op, RegStorage r_base, int disp) {
  LOG(FATAL) << "Unexpected use of OpMem for Arm64";
  return NULL;
}

LIR* Arm64Mir2Lir::StoreBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                        int displacement, RegStorage r_src, OpSize size) {
  LOG(FATAL) << "Unexpected use of StoreBaseIndexedDisp for Arm64";
  return NULL;
}

LIR* Arm64Mir2Lir::OpRegMem(OpKind op, RegStorage r_dest, RegStorage r_base, int offset) {
  LOG(FATAL) << "Unexpected use of OpRegMem for Arm64";
  return NULL;
}

LIR* Arm64Mir2Lir::LoadBaseIndexedDisp(RegStorage r_base, RegStorage r_index, int scale,
                                       int displacement, RegStorage r_dest, OpSize size) {
  LOG(FATAL) << "Unexpected use of LoadBaseIndexedDisp for Arm64";
  return NULL;
}

}  // namespace art
