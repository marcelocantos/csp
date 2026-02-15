#include <csp/stack_analysis.h>

#if defined(__aarch64__)

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace csp {

namespace {

// --- Expression tree (transient IR) ---

struct expr {
    enum kind_t { CONST, MAX, ADD, CALL_DIRECT, CALL_INDIRECT, BUDGET };
    kind_t kind;
    size_t value = 0;              // CONST, BUDGET: depth; CALL_INDIRECT: offset
    const void* target = nullptr;  // CALL_DIRECT: callee address
    std::shared_ptr<expr> left, right;

    static std::shared_ptr<expr> make_const(size_t v) {
        auto e = std::make_shared<expr>();
        e->kind = CONST;
        e->value = v;
        return e;
    }
    static std::shared_ptr<expr> make_budget(size_t v) {
        auto e = std::make_shared<expr>();
        e->kind = BUDGET;
        e->value = v;
        return e;
    }
    static std::shared_ptr<expr> make_max(std::shared_ptr<expr> a,
                                          std::shared_ptr<expr> b) {
        auto e = std::make_shared<expr>();
        e->kind = MAX;
        e->left = std::move(a);
        e->right = std::move(b);
        return e;
    }
    static std::shared_ptr<expr> make_add(std::shared_ptr<expr> a,
                                          std::shared_ptr<expr> b) {
        auto e = std::make_shared<expr>();
        e->kind = ADD;
        e->left = std::move(a);
        e->right = std::move(b);
        return e;
    }
    static std::shared_ptr<expr> make_call_direct(const void* addr) {
        auto e = std::make_shared<expr>();
        e->kind = CALL_DIRECT;
        e->target = addr;
        return e;
    }
    static std::shared_ptr<expr> make_call_indirect(size_t offset) {
        auto e = std::make_shared<expr>();
        e->kind = CALL_INDIRECT;
        e->value = offset;
        return e;
    }
};

// --- Expression simplification ---

// Returns true if the expression is a pure constant (no CALL_INDIRECT or BUDGET).
bool is_pure(const expr& e) {
    switch (e.kind) {
    case expr::CONST: return true;
    case expr::BUDGET: return false;
    case expr::CALL_INDIRECT: return false;
    case expr::CALL_DIRECT: return false; // might resolve to non-pure
    case expr::MAX:
    case expr::ADD:
        return is_pure(*e.left) && is_pure(*e.right);
    }
    return false;
}

// Evaluate a pure expression to a constant.
size_t eval_pure(const expr& e) {
    switch (e.kind) {
    case expr::CONST: return e.value;
    case expr::MAX: return std::max(eval_pure(*e.left), eval_pure(*e.right));
    case expr::ADD: return eval_pure(*e.left) + eval_pure(*e.right);
    default: assert(false); return 0;
    }
}

// Simplify an expression: collapse pure subtrees to constants.
std::shared_ptr<expr> simplify(std::shared_ptr<expr> e) {
    if (!e) return e;
    if (e->kind == expr::MAX || e->kind == expr::ADD) {
        e->left = simplify(std::move(e->left));
        e->right = simplify(std::move(e->right));
    }
    if (is_pure(*e)) {
        return expr::make_const(eval_pure(*e));
    }
    return e;
}

// --- Bytecode VM ---

enum opcode : uint8_t {
    OP_PUSH           = 0x01,
    OP_MAX            = 0x02,
    OP_ADD            = 0x03,
    OP_CALL_DIRECT    = 0x04,
    OP_CALL_INDIRECT  = 0x05,
    OP_BUDGET         = 0x06,
};

void emit_u64(std::vector<uint8_t>& prog, uint64_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    prog.insert(prog.end(), p, p + 8);
}

void emit_ptr(std::vector<uint8_t>& prog, const void* v) {
    emit_u64(prog, reinterpret_cast<uint64_t>(v));
}

uint64_t read_u64(const uint8_t*& ip) {
    uint64_t v;
    std::memcpy(&v, ip, 8);
    ip += 8;
    return v;
}

const void* read_ptr(const uint8_t*& ip) {
    return reinterpret_cast<const void*>(read_u64(ip));
}

// Compile expression tree to bytecode (post-order traversal).
void compile(const expr& e, std::vector<uint8_t>& prog) {
    switch (e.kind) {
    case expr::CONST:
        prog.push_back(OP_PUSH);
        emit_u64(prog, e.value);
        break;
    case expr::BUDGET:
        prog.push_back(OP_BUDGET);
        emit_u64(prog, e.value);
        break;
    case expr::MAX:
        compile(*e.left, prog);
        compile(*e.right, prog);
        prog.push_back(OP_MAX);
        break;
    case expr::ADD:
        compile(*e.left, prog);
        compile(*e.right, prog);
        prog.push_back(OP_ADD);
        break;
    case expr::CALL_DIRECT:
        prog.push_back(OP_CALL_DIRECT);
        emit_ptr(prog, e.target);
        break;
    case expr::CALL_INDIRECT:
        prog.push_back(OP_CALL_INDIRECT);
        emit_u64(prog, e.value);
        break;
    }
}

// --- Program cache ---

std::mutex g_cache_mu;
std::unordered_map<const void*, std::vector<uint8_t>> g_cache;

// Forward declaration.
std::vector<uint8_t> analyze_and_compile(const void* fn,
                                         const stack_analysis_options& opts,
                                         int depth,
                                         std::set<const void*>& in_progress,
                                         size_t& inst_count);

const std::vector<uint8_t>& get_or_compile(const void* fn,
                                            const stack_analysis_options& opts,
                                            int depth,
                                            std::set<const void*>& in_progress,
                                            size_t& inst_count) {
    {
        std::lock_guard<std::mutex> lk(g_cache_mu);
        auto it = g_cache.find(fn);
        if (it != g_cache.end()) return it->second;
    }
    auto prog = analyze_and_compile(fn, opts, depth, in_progress, inst_count);
    std::lock_guard<std::mutex> lk(g_cache_mu);
    auto [it, _] = g_cache.emplace(fn, std::move(prog));
    return it->second;
}

// --- ARM64 instruction decoding ---

// Instruction matching helpers.
inline bool match(uint32_t inst, uint32_t mask, uint32_t value) {
    return (inst & mask) == value;
}

inline int64_t sign_extend(uint32_t val, int bits) {
    int64_t v = val;
    int64_t sign_bit = 1LL << (bits - 1);
    return (v ^ sign_bit) - sign_bit;
}

// --- Register tracking ---

struct reg_state {
    enum origin_t { UNKNOWN, DATA_OFFSET };
    origin_t origin = UNKNOWN;
    size_t offset = 0;
};

struct analysis_state {
    const uint32_t* pc;
    int64_t sp_delta;
    reg_state regs[32];
};

// --- Instruction walker ---

// Visited set entry.
struct visit_key {
    const uint32_t* pc;
    int64_t sp_delta;
    bool operator<(const visit_key& o) const {
        return pc < o.pc || (pc == o.pc && sp_delta < o.sp_delta);
    }
};

std::shared_ptr<expr> walk(analysis_state state,
                           const stack_analysis_options& opts,
                           int call_depth,
                           std::set<const void*>& in_progress,
                           std::set<visit_key>& visited,
                           size_t& inst_count,
                           int branch_depth = 0) {
    static constexpr int MAX_BRANCH_DEPTH = 64;
    std::shared_ptr<expr> result = expr::make_const(0);
    size_t high_water = 0;  // max |sp_delta| seen on this path

    while (true) {
        // Safety: stop if PC is null or misaligned.
        if (!state.pc || ((uintptr_t)state.pc & 3) != 0) {
            return expr::make_max(std::move(result),
                                  expr::make_const(high_water));
        }
        // Track whether we've exceeded our budget. When over budget,
        // we continue the sequential walk (to reach RET/BLR/BR) but
        // stop exploring branch targets and callee bodies.
        bool over_budget = inst_count >= opts.max_instructions ||
                           branch_depth >= MAX_BRANCH_DEPTH;
        inst_count++;

        visit_key vk{state.pc, state.sp_delta};
        if (!visited.insert(vk).second) {
            // Back-edge: loop detected, terminate path.
            return expr::make_max(std::move(result),
                                  expr::make_const(high_water));
        }

        uint32_t inst = *state.pc;
        size_t cur_depth = static_cast<size_t>(
            state.sp_delta < 0 ? -state.sp_delta : state.sp_delta);
        if (cur_depth > high_water) high_water = cur_depth;

        // --- SUB SP, SP, #imm ---
        if (match(inst, 0xFF0003FF, 0xD10003FF)) {
            uint32_t imm12 = (inst >> 10) & 0xFFF;
            uint32_t shift = (inst >> 22) & 0x3;
            int64_t imm = imm12;
            if (shift == 1) imm <<= 12;
            state.sp_delta -= imm;
            state.pc++;
            continue;
        }

        // --- ADD SP, SP, #imm ---
        if (match(inst, 0xFF0003FF, 0x910003FF)) {
            uint32_t imm12 = (inst >> 10) & 0xFFF;
            uint32_t shift = (inst >> 22) & 0x3;
            int64_t imm = imm12;
            if (shift == 1) imm <<= 12;
            state.sp_delta += imm;
            state.pc++;
            continue;
        }

        // --- STP pre-indexed [SP, #imm]! (64-bit GP) ---
        if (match(inst, 0xFFC003E0, 0xA9800000 | (31 << 5))) {
            int32_t imm7 = (inst >> 15) & 0x7F;
            int64_t offset = sign_extend(imm7, 7) * 8;
            state.sp_delta += offset;
            // Update dest register tracking: STP writes two regs from stack,
            // but the registers being stored aren't modified. No reg tracking
            // change needed — only SP changes via writeback.
            state.pc++;
            continue;
        }

        // --- LDP post-indexed [SP], #imm (64-bit GP) ---
        if (match(inst, 0xFFC003E0, 0xA8C00000 | (31 << 5))) {
            int32_t imm7 = (inst >> 15) & 0x7F;
            int64_t offset = sign_extend(imm7, 7) * 8;
            state.sp_delta += offset;
            // Mark loaded registers as unknown.
            uint32_t rt = inst & 0x1F;
            uint32_t rt2 = (inst >> 10) & 0x1F;
            if (rt < 31) state.regs[rt].origin = reg_state::UNKNOWN;
            if (rt2 < 31) state.regs[rt2].origin = reg_state::UNKNOWN;
            state.pc++;
            continue;
        }

        // --- RET ---
        if (match(inst, 0xFFFFFC1F, 0xD65F0000)) {
            return expr::make_max(std::move(result),
                                  expr::make_const(high_water));
        }

        // --- BR Xn (indirect tail call / indirect branch) ---
        if (match(inst, 0xFFFFFC1F, 0xD61F0000)) {
            uint32_t rn = (inst >> 5) & 0x1F;

            std::shared_ptr<expr> callee;
            if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                callee = expr::make_call_indirect(state.regs[rn].offset);
            } else {
                callee = expr::make_budget(opts.indirect_call_budget);
            }

            auto call_expr = expr::make_add(expr::make_const(cur_depth),
                                            std::move(callee));
            return expr::make_max(std::move(result),
                                  expr::make_max(expr::make_const(high_water),
                                                 std::move(call_expr)));
        }

        // --- BL offset ---
        if (match(inst, 0xFC000000, 0x94000000)) {
            int64_t imm26 = sign_extend(inst & 0x3FFFFFF, 26);
            const void* target = state.pc + imm26;

            std::shared_ptr<expr> callee;
            if (!over_budget && call_depth > 0 && !in_progress.count(target)) {
                const auto& prog = get_or_compile(target, opts, call_depth - 1, in_progress, inst_count);
                // If the program is a simple PUSH (9 bytes), extract the constant.
                if (prog.size() == 9 && prog[0] == OP_PUSH) {
                    const uint8_t* p = prog.data() + 1;
                    callee = expr::make_const(read_u64(p));
                } else {
                    callee = expr::make_call_direct(target);
                }
            } else {
                callee = expr::make_budget(opts.indirect_call_budget);
            }

            auto call_expr = expr::make_add(expr::make_const(cur_depth),
                                            std::move(callee));
            result = expr::make_max(std::move(result), std::move(call_expr));

            // Continue after the call.
            state.pc++;
            continue;
        }

        // --- BLR Xn ---
        if (match(inst, 0xFFFFFC1F, 0xD63F0000)) {
            uint32_t rn = (inst >> 5) & 0x1F;

            std::shared_ptr<expr> callee;
            if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                callee = expr::make_call_indirect(state.regs[rn].offset);
            } else {
                callee = expr::make_budget(opts.indirect_call_budget);
            }

            auto call_expr = expr::make_add(expr::make_const(cur_depth),
                                            std::move(callee));
            result = expr::make_max(std::move(result), std::move(call_expr));

            // Continue after the call.
            state.pc++;
            continue;
        }

        // --- B offset (unconditional branch) ---
        if (match(inst, 0xFC000000, 0x14000000)) {
            int64_t imm26 = sign_extend(inst & 0x3FFFFFF, 26);
            state.pc = state.pc + imm26;
            continue;
        }

        // --- B.cond ---
        if (match(inst, 0xFF000010, 0x54000000)) {
            if (over_budget) {
                // Over budget: only follow fall-through to keep reaching
                // instructions on the main path (e.g., BLR, RET).
                state.pc++;
                continue;
            }
            int64_t imm19 = sign_extend((inst >> 5) & 0x7FFFF, 19);
            const uint32_t* target = state.pc + imm19;

            analysis_state taken_state = state;
            taken_state.pc = target;
            auto taken = walk(taken_state, opts, call_depth, in_progress,
                              visited, inst_count, branch_depth + 1);

            state.pc++;
            auto not_taken = walk(state, opts, call_depth, in_progress,
                                  visited, inst_count, branch_depth + 1);

            return expr::make_max(std::move(result),
                                  expr::make_max(std::move(taken),
                                                 std::move(not_taken)));
        }

        // --- CBZ / CBNZ ---
        if (match(inst, 0x7F000000, 0x34000000) ||
            match(inst, 0x7F000000, 0x35000000)) {
            if (over_budget) {
                state.pc++;
                continue;
            }
            int64_t imm19 = sign_extend((inst >> 5) & 0x7FFFF, 19);
            const uint32_t* target = state.pc + imm19;

            analysis_state taken_state = state;
            taken_state.pc = target;
            auto taken = walk(taken_state, opts, call_depth, in_progress,
                              visited, inst_count, branch_depth + 1);

            state.pc++;
            auto not_taken = walk(state, opts, call_depth, in_progress,
                                  visited, inst_count, branch_depth + 1);

            return expr::make_max(std::move(result),
                                  expr::make_max(std::move(taken),
                                                 std::move(not_taken)));
        }

        // --- TBZ / TBNZ ---
        if (match(inst, 0x7F000000, 0x36000000) ||
            match(inst, 0x7F000000, 0x37000000)) {
            if (over_budget) {
                state.pc++;
                continue;
            }
            int64_t imm14 = sign_extend((inst >> 5) & 0x3FFF, 14);
            const uint32_t* target = state.pc + imm14;

            analysis_state taken_state = state;
            taken_state.pc = target;
            auto taken = walk(taken_state, opts, call_depth, in_progress,
                              visited, inst_count, branch_depth + 1);

            state.pc++;
            auto not_taken = walk(state, opts, call_depth, in_progress,
                                  visited, inst_count, branch_depth + 1);

            return expr::make_max(std::move(result),
                                  expr::make_max(std::move(taken),
                                                 std::move(not_taken)));
        }

        // --- SUB SP, SP, Xn (register-based stack adjustment) ---
        if (match(inst, 0xFF0003FF, 0xCB0003FF)) {
            return expr::make_max(std::move(result),
                                  expr::make_budget(opts.indirect_call_budget));
        }

        // --- Register tracking: LDR Xt, [Xn, #imm] (unsigned offset) ---
        if (match(inst, 0xFFC00000, 0xF9400000)) {
            uint32_t rt = inst & 0x1F;
            uint32_t rn = (inst >> 5) & 0x1F;
            uint32_t imm12 = (inst >> 10) & 0xFFF;
            size_t byte_offset = imm12 * 8; // scaled by 8 for 64-bit LDR

            if (rt < 31) {
                if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                    state.regs[rt].origin = reg_state::DATA_OFFSET;
                    state.regs[rt].offset = state.regs[rn].offset + byte_offset;
                } else if (rn == 31) {
                    // LDR from SP — not data-derived.
                    state.regs[rt].origin = reg_state::UNKNOWN;
                } else {
                    state.regs[rt].origin = reg_state::UNKNOWN;
                }
            }
            state.pc++;
            continue;
        }

        // --- Register tracking: ADD Xd, Xn, #imm ---
        // Note: must check AFTER ADD SP,SP,#imm (which has Rd=Rn=SP).
        if (match(inst, 0xFF000000, 0x91000000)) {
            uint32_t rd = inst & 0x1F;
            uint32_t rn = (inst >> 5) & 0x1F;
            uint32_t imm12 = (inst >> 10) & 0xFFF;
            uint32_t shift = (inst >> 22) & 0x3;
            size_t imm = imm12;
            if (shift == 1) imm <<= 12;

            if (rd < 31) {
                if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                    state.regs[rd].origin = reg_state::DATA_OFFSET;
                    state.regs[rd].offset = state.regs[rn].offset + imm;
                } else {
                    state.regs[rd].origin = reg_state::UNKNOWN;
                }
            }
            state.pc++;
            continue;
        }

        // --- Register tracking: MOV Xd, Xm (ORR Xd, XZR, Xm) ---
        if (match(inst, 0xFFE0FFE0, 0xAA0003E0)) {
            uint32_t rd = inst & 0x1F;
            uint32_t rm = (inst >> 16) & 0x1F;
            if (rd < 31) {
                if (rm < 31) {
                    state.regs[rd] = state.regs[rm];
                } else {
                    state.regs[rd].origin = reg_state::UNKNOWN;
                }
            }
            state.pc++;
            continue;
        }

        // --- Any other instruction: check if it writes a GP register ---
        // Conservative: mark destination register as unknown for common
        // instruction formats that write to Rd at bits [4:0].
        {
            uint32_t rd = inst & 0x1F;
            if (rd < 31) {
                // Only clear if this looks like it writes Rd.
                // Most data-processing instructions do. We conservatively
                // clear for any unrecognized instruction to avoid false
                // DATA_OFFSET propagation.
                state.regs[rd].origin = reg_state::UNKNOWN;
            }
        }

        state.pc++;
    }

    return result;
}

// Analyze a function and compile to bytecode.
// inst_count is shared across all callees so the global budget is respected.
std::vector<uint8_t> analyze_and_compile(const void* fn,
                                         const stack_analysis_options& opts,
                                         int depth,
                                         std::set<const void*>& in_progress,
                                         size_t& inst_count) {
    if (in_progress.count(fn) || inst_count >= opts.max_instructions) {
        // Recursive call or budget exhausted — use budget.
        std::vector<uint8_t> prog;
        prog.push_back(OP_BUDGET);
        emit_u64(prog, opts.indirect_call_budget);
        return prog;
    }
    in_progress.insert(fn);

    analysis_state initial{};
    initial.pc = static_cast<const uint32_t*>(fn);
    initial.sp_delta = 0;
    // X0 = data parameter.
    initial.regs[0].origin = reg_state::DATA_OFFSET;
    initial.regs[0].offset = 0;

    std::set<visit_key> visited;
    auto tree = walk(initial, opts, depth, in_progress, visited, inst_count);

    tree = simplify(std::move(tree));

    std::vector<uint8_t> prog;
    compile(*tree, prog);

    in_progress.erase(fn);
    return prog;
}

// Evaluate a compiled program.
static constexpr int EVAL_STACK_SIZE = 256;

stack_analysis eval_program(const std::vector<uint8_t>& program,
                            const void* data,
                            const stack_analysis_options& opts) {
    size_t stack[EVAL_STACK_SIZE];
    int sp = 0;
    bool is_exact = true;
    const uint8_t* ip = program.data();
    const uint8_t* end = ip + program.size();

    std::set<const void*> in_progress;

    while (ip < end) {
        if (sp >= EVAL_STACK_SIZE - 1) {
            // Stack overflow — return what we have so far.
            is_exact = false;
            break;
        }
        switch (*ip++) {
        case OP_PUSH:
            stack[sp++] = read_u64(ip);
            break;
        case OP_MAX: {
            if (sp < 2) break;
            auto b = stack[--sp], a = stack[--sp];
            stack[sp++] = std::max(a, b);
            break;
        }
        case OP_ADD: {
            if (sp < 2) break;
            auto b = stack[--sp], a = stack[--sp];
            stack[sp++] = a + b;
            break;
        }
        case OP_CALL_DIRECT: {
            auto addr = read_ptr(ip);
            size_t eval_inst_count = 0;
            const auto& callee = get_or_compile(addr, opts, opts.max_call_depth,
                                                 in_progress, eval_inst_count);
            auto r = eval_program(callee, nullptr, opts);
            is_exact &= r.is_exact;
            stack[sp++] = r.max_depth;
            break;
        }
        case OP_CALL_INDIRECT: {
            auto off = read_u64(ip);
            if (!data) {
                is_exact = false;
                stack[sp++] = opts.indirect_call_budget;
            } else {
                auto target = *reinterpret_cast<const void* const*>(
                    static_cast<const char*>(data) + off);
                size_t eval_inst_count = 0;
                const auto& callee = get_or_compile(target, opts,
                                                     opts.max_call_depth,
                                                     in_progress, eval_inst_count);
                auto r = eval_program(callee, nullptr, opts);
                is_exact &= r.is_exact;
                stack[sp++] = r.max_depth;
            }
            break;
        }
        case OP_BUDGET:
            is_exact = false;
            stack[sp++] = read_u64(ip);
            break;
        }
    }

    return {sp > 0 ? stack[0] : 0, is_exact};
}

} // anonymous namespace

stack_analysis analyze_stack_depth(const void* fn, const void* data,
                                  stack_analysis_options opts) {
    std::set<const void*> in_progress;
    size_t inst_count = 0;
    const auto& program = get_or_compile(fn, opts, opts.max_call_depth,
                                          in_progress, inst_count);
    return eval_program(program, data, opts);
}

} // namespace csp

#else // !__aarch64__

namespace csp {

stack_analysis analyze_stack_depth(const void* fn, const void* data,
                                  stack_analysis_options opts) {
    // Non-ARM64: return conservative default.
    return {32u << 10, false};
}

} // namespace csp

#endif
