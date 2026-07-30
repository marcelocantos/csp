#include <csp/stack_analysis.h>

#if defined(__aarch64__)

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// 🎯T52.4: the analysis worker consults the profile high-water table
// (defined in src/csp.cc) to refine the indirect-call budget. Local
// declarations avoid pulling the full csp_internal.h into this TU; the
// alias is identical to the one in csp.h, so the amalgamated single-TU
// build sees one consistent declaration.
namespace csp::internal {
using EntryFn = void (*)(void*);
} // namespace csp::internal
namespace csp::detail {
size_t get_stack_high_water(::csp::internal::EntryFn fn);
} // namespace csp::detail

namespace csp {

namespace {

// --- Inline containers (no BLR instructions) ---
//
// std::unordered_map and std::set generate BLR (indirect call) instructions
// for hash dispatch and allocator calls.  When the stack analyzer walks
// through its own code (bootstrapping), those BLRs make every result
// inexact.  These replacements compile to pure inline arithmetic.

// Open-addressing hash map keyed by const void*.
template <typename V>
class ptr_map {
    struct slot {
        const void* key;
        V value;
        bool occupied;
    };
    slot* data_;
    size_t cap_;
    size_t count_;

    static size_t hash(const void* p) {
        return reinterpret_cast<uintptr_t>(p) * 0x9E3779B97F4A7C15ULL;
    }

    void grow() {
        size_t old_cap = cap_;
        auto* old = data_;
        cap_ *= 2;
        data_ = new slot[cap_]();
        count_ = 0;
        for (size_t i = 0; i < old_cap; i++) {
            if (old[i].occupied) {
                size_t j = hash(old[i].key) & (cap_ - 1);
                while (data_[j].occupied) j = (j + 1) & (cap_ - 1);
                data_[j] = {old[i].key, std::move(old[i].value), true};
                count_++;
            }
        }
        delete[] old;
    }

public:
    ptr_map() : data_(new slot[16]()), cap_(16), count_(0) {}
    ~ptr_map() { delete[] data_; }
    ptr_map(const ptr_map&) = delete;
    ptr_map& operator=(const ptr_map&) = delete;

    V* find(const void* key) {
        size_t i = hash(key) & (cap_ - 1);
        while (data_[i].occupied) {
            if (data_[i].key == key) return &data_[i].value;
            i = (i + 1) & (cap_ - 1);
        }
        return nullptr;
    }

    // Insert if absent. Returns true if newly inserted.
    bool emplace(const void* key, V val) {
        if (count_ * 4 >= cap_ * 3) grow();
        size_t i = hash(key) & (cap_ - 1);
        while (data_[i].occupied) {
            if (data_[i].key == key) return false;
            i = (i + 1) & (cap_ - 1);
        }
        data_[i] = {key, std::move(val), true};
        count_++;
        return true;
    }
};

// Open-addressing hash set for {pc, sp_delta} visit keys (insert-only).
class visit_set {
    struct slot {
        const uint32_t* pc;
        int64_t sp_delta;
        bool occupied;
    };
    slot* data_;
    size_t cap_;
    size_t count_;

    static size_t hash(const uint32_t* pc, int64_t sp_delta) {
        auto h = reinterpret_cast<uintptr_t>(pc) * 0x9E3779B97F4A7C15ULL;
        h ^= static_cast<size_t>(sp_delta) * 0x517CC1B727220A95ULL;
        return h;
    }

    void grow() {
        size_t old_cap = cap_;
        auto* old = data_;
        cap_ *= 2;
        data_ = new slot[cap_]();
        count_ = 0;
        for (size_t i = 0; i < old_cap; i++) {
            if (old[i].occupied) {
                size_t j = hash(old[i].pc, old[i].sp_delta) & (cap_ - 1);
                while (data_[j].occupied) j = (j + 1) & (cap_ - 1);
                data_[j] = {old[i].pc, old[i].sp_delta, true};
                count_++;
            }
        }
        delete[] old;
    }

public:
    visit_set() : data_(new slot[256]()), cap_(256), count_(0) {}
    ~visit_set() { delete[] data_; }
    visit_set(const visit_set&) = delete;
    visit_set& operator=(const visit_set&) = delete;

    // Returns true if newly inserted.
    bool insert(const uint32_t* pc, int64_t sp_delta) {
        if (count_ * 4 >= cap_ * 3) grow();
        size_t i = hash(pc, sp_delta) & (cap_ - 1);
        while (data_[i].occupied) {
            if (data_[i].pc == pc && data_[i].sp_delta == sp_delta) return false;
            i = (i + 1) & (cap_ - 1);
        }
        data_[i] = {pc, sp_delta, true};
        count_++;
        return true;
    }
};

// Fixed-capacity linear-scan set for cycle detection (bounded by call depth).
//
// 🎯T52.1: `limit` wires stack_analysis_options::max_call_depth to the
// on-stack capacity (clamped to the compile-time MAX). insert() reports
// failure when the set is full; the caller must treat a failed insert
// exactly like a detected cycle — a silently dropped entry would let a
// cycle among un-inserted functions evade contains() and degrade cycle
// detection past the capacity.
class small_ptr_set {
    static constexpr int MAX = 64;
    const void* data_[MAX];
    int size_ = 0;
    int limit_;
public:
    explicit small_ptr_set(int limit = MAX)
        : limit_(limit < 1 ? 1 : (limit > MAX ? MAX : limit)) {}
    bool contains(const void* p) const {
        for (int i = 0; i < size_; i++)
            if (data_[i] == p) return true;
        return false;
    }
    // Returns false (and does not insert) when the set is at capacity.
    [[nodiscard]] bool insert(const void* p) {
        if (size_ >= limit_) return false;
        data_[size_++] = p;
        return true;
    }
    void erase(const void* p) {
        for (int i = 0; i < size_; i++) {
            if (data_[i] == p) {
                data_[i] = data_[--size_];
                return;
            }
        }
    }
};

// --- Intrusive reference-counted pointer ---
//
// Replaces expr_ptr to eliminate BLR instructions from
// virtual dispatch in shared_ptr's control block destructor.  The
// destructor is a direct `delete` (BL), not a virtual call (BLR).

struct expr;

class expr_ptr {
    expr* p_ = nullptr;
    void addref();
    void release();
public:
    expr_ptr() = default;
    explicit expr_ptr(expr* p) : p_(p) { addref(); }
    expr_ptr(const expr_ptr& o) : p_(o.p_) { addref(); }
    expr_ptr(expr_ptr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~expr_ptr() { release(); }
    expr_ptr& operator=(const expr_ptr& o) {
        if (p_ != o.p_) { release(); p_ = o.p_; addref(); }
        return *this;
    }
    expr_ptr& operator=(expr_ptr&& o) noexcept {
        if (this != &o) { release(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    expr* get() const { return p_; }
    expr& operator*() const { return *p_; }
    expr* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
};

// --- Expression tree (transient IR) ---

// 🎯T3.10: inbound argument-register provenance forwarded across a BL
// boundary. One entry per X0–X7 register whose provenance the BL site
// resolved, so the callee's analyser can seed that register instead of
// blindly assuming the data pointer lives in X0. `origin` stores a
// reg_state::origin_t value (reg_state is defined further down, so this
// POD keeps it as a raw byte the BL handler and evaluator convert at the
// boundary).
struct arg_prov {
    uint8_t  reg;       // argument register index (0–7)
    uint8_t  origin;    // static_cast<uint8_t>(reg_state::origin_t)
    uint64_t payload;   // DATA_OFFSET: byte offset into the caller's data;
                        // PC_RELATIVE: absolute address bits;
                        // CONST: signed value bits
};

struct expr {
    enum kind_t {
        CONST,
        MAX,
        ADD,
        CALL_DIRECT,             // direct call (resolved at walk time)
        CALL_INDIRECT,           // indirect call through data offset
        BUDGET,                  // conservative fallback budget
        CALL_DIRECT_WITH_DATA,   // direct call forwarding a data sub-pointer
        CALL_DIRECT_WITH_ARGS,   // 🎯T3.10: direct call forwarding X0–X7 provenance
    };
    kind_t kind;
    size_t value = 0;              // CONST, BUDGET: depth; CALL_INDIRECT/CALL_DIRECT_WITH_DATA: offset
    const void* target = nullptr;  // CALL_DIRECT, CALL_DIRECT_WITH_DATA: callee address
    int refcount_ = 0;
    expr_ptr left, right;
    std::vector<arg_prov> args;    // CALL_DIRECT_WITH_ARGS: forwarded register seeds

    static expr_ptr make_const(size_t v) {
        auto* e = new expr();
        e->kind = CONST;
        e->value = v;
        return expr_ptr(e);
    }
    static expr_ptr make_budget(size_t v) {
        auto* e = new expr();
        e->kind = BUDGET;
        e->value = v;
        return expr_ptr(e);
    }
    static expr_ptr make_max(expr_ptr a, expr_ptr b) {
        auto* e = new expr();
        e->kind = MAX;
        e->left = std::move(a);
        e->right = std::move(b);
        return expr_ptr(e);
    }
    static expr_ptr make_add(expr_ptr a, expr_ptr b) {
        auto* e = new expr();
        e->kind = ADD;
        e->left = std::move(a);
        e->right = std::move(b);
        return expr_ptr(e);
    }
    static expr_ptr make_call_direct(const void* addr) {
        auto* e = new expr();
        e->kind = CALL_DIRECT;
        e->target = addr;
        return expr_ptr(e);
    }
    static expr_ptr make_call_indirect(size_t offset) {
        auto* e = new expr();
        e->kind = CALL_INDIRECT;
        e->value = offset;
        return expr_ptr(e);
    }
    // Direct call that also forwards a data sub-pointer (offset into current data)
    // as the callee's data argument for interprocedural context propagation.
    static expr_ptr make_call_direct_with_data(const void* addr, size_t data_off) {
        auto* e = new expr();
        e->kind = CALL_DIRECT_WITH_DATA;
        e->target = addr;
        e->value = data_off;
        return expr_ptr(e);
    }
    // 🎯T3.10: direct call forwarding the inbound provenance of one or more
    // argument registers (a DATA_OFFSET sub-pointer plus any PC_RELATIVE /
    // CONST argument registers) so the callee's analyser can resolve calls
    // and prune branches through arguments that arrive in X1–X7.
    static expr_ptr make_call_direct_with_args(const void* addr,
                                               std::vector<arg_prov> seeds) {
        auto* e = new expr();
        e->kind = CALL_DIRECT_WITH_ARGS;
        e->target = addr;
        e->args = std::move(seeds);
        return expr_ptr(e);
    }
};

void expr_ptr::addref() { if (p_) p_->refcount_++; }
void expr_ptr::release() { if (p_ && --p_->refcount_ == 0) delete p_; }

// --- Expression operations (iterative to avoid stack overflow) ---

// Returns true if the expression is a pure constant (no CALL_INDIRECT or BUDGET).
bool is_pure(const expr& root) {
    std::vector<const expr*> stack;
    stack.push_back(&root);
    while (!stack.empty()) {
        auto* e = stack.back();
        stack.pop_back();
        switch (e->kind) {
        case expr::CONST: break;
        case expr::BUDGET:
        case expr::CALL_INDIRECT:
        case expr::CALL_DIRECT:
        case expr::CALL_DIRECT_WITH_DATA:
        case expr::CALL_DIRECT_WITH_ARGS: return false;
        case expr::MAX:
        case expr::ADD:
            stack.push_back(e->left.get());
            stack.push_back(e->right.get());
            break;
        }
    }
    return true;
}

// Evaluate a pure expression to a constant (iterative post-order).
size_t eval_pure(const expr& root) {
    struct frame { const expr* e; int phase; };
    std::vector<frame> traversal;
    std::vector<size_t> values;
    traversal.push_back({&root, 0});
    while (!traversal.empty()) {
        auto& f = traversal.back();
        switch (f.e->kind) {
        case expr::CONST:
            values.push_back(f.e->value);
            traversal.pop_back();
            break;
        case expr::MAX:
        case expr::ADD:
            if (f.phase == 0) {
                f.phase = 1;
                traversal.push_back({f.e->left.get(), 0});
            } else if (f.phase == 1) {
                f.phase = 2;
                traversal.push_back({f.e->right.get(), 0});
            } else {
                auto b = values.back(); values.pop_back();
                auto a = values.back(); values.pop_back();
                values.push_back(f.e->kind == expr::MAX ? std::max(a, b) : a + b);
                traversal.pop_back();
            }
            break;
        default:
            __builtin_unreachable();
        }
    }
    return values.empty() ? 0 : values.back();
}

// --- Bytecode VM ---

enum opcode : uint8_t {
    OP_PUSH                  = 0x01,
    OP_MAX                   = 0x02,
    OP_ADD                   = 0x03,
    OP_CALL_DIRECT           = 0x04,
    OP_CALL_INDIRECT         = 0x05,
    OP_BUDGET                = 0x06,
    OP_CALL_DIRECT_WITH_DATA = 0x07,  // <target_addr:8> <data_prov:8>
    // 🎯T3.10: <target_addr:8> <count:1> count×<reg:1 origin:1 payload:8>
    OP_CALL_DIRECT_WITH_ARGS = 0x08,
};

// 🎯T3.10: OP_CALL_DIRECT_WITH_DATA's 64-bit operand packs the forwarding
// register index into the high 5 bits and the byte offset into the low 59.
// A closure is at most a few hundred bytes, so 59 bits of offset is ample
// headroom, and reg index 0 reproduces the pre-T3.10 offset-only bytes
// exactly (backward compatible — the common X0 forward is byte-identical).
constexpr int      kDataRegShift = 59;
constexpr uint64_t kDataOffMask  = (uint64_t{1} << kDataRegShift) - 1;
constexpr uint64_t pack_data_prov(int reg, uint64_t off) {
    return (static_cast<uint64_t>(reg) << kDataRegShift) | (off & kDataOffMask);
}
constexpr int      unpack_data_reg(uint64_t v) { return static_cast<int>(v >> kDataRegShift); }
constexpr uint64_t unpack_data_off(uint64_t v) { return v & kDataOffMask; }

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

// Compile expression tree to bytecode (iterative post-order traversal).
void compile(const expr& root, std::vector<uint8_t>& prog) {
    struct frame { const expr* e; int phase; };
    std::vector<frame> stack;
    stack.push_back({&root, 0});
    while (!stack.empty()) {
        auto& f = stack.back();
        switch (f.e->kind) {
        case expr::CONST:
            prog.push_back(OP_PUSH);
            emit_u64(prog, f.e->value);
            stack.pop_back();
            break;
        case expr::BUDGET:
            prog.push_back(OP_BUDGET);
            emit_u64(prog, f.e->value);
            stack.pop_back();
            break;
        case expr::CALL_DIRECT:
            prog.push_back(OP_CALL_DIRECT);
            emit_ptr(prog, f.e->target);
            stack.pop_back();
            break;
        case expr::CALL_INDIRECT:
            prog.push_back(OP_CALL_INDIRECT);
            emit_u64(prog, f.e->value);
            stack.pop_back();
            break;
        case expr::CALL_DIRECT_WITH_DATA:
            prog.push_back(OP_CALL_DIRECT_WITH_DATA);
            emit_ptr(prog, f.e->target);
            emit_u64(prog, f.e->value);  // packed (reg_index, data_offset)
            stack.pop_back();
            break;
        case expr::CALL_DIRECT_WITH_ARGS:
            prog.push_back(OP_CALL_DIRECT_WITH_ARGS);
            emit_ptr(prog, f.e->target);
            prog.push_back(static_cast<uint8_t>(f.e->args.size()));
            for (const auto& a : f.e->args) {
                prog.push_back(a.reg);
                prog.push_back(a.origin);
                emit_u64(prog, a.payload);
            }
            stack.pop_back();
            break;
        case expr::MAX:
        case expr::ADD:
            if (f.phase == 0) {
                f.phase = 1;
                stack.push_back({f.e->left.get(), 0});
            } else if (f.phase == 1) {
                f.phase = 2;
                stack.push_back({f.e->right.get(), 0});
            } else {
                prog.push_back(f.e->kind == expr::MAX ? OP_MAX : OP_ADD);
                stack.pop_back();
            }
            break;
        }
    }
}

// --- Spinlock (pure inline atomics, no BLR) ---

class spinlock {
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
public:
    void lock() { while (flag_.test_and_set(std::memory_order_acquire)) { } }
    void unlock() { flag_.clear(std::memory_order_release); }
};

// --- Register tracking ---
//
// Defined here (above the program cache and analyser entry) so the cache
// and the callee-entry seeding can talk about register provenance: a
// callable forwarded across a BL boundary seeds the register it arrives in
// (🎯T3.10), not blindly X0.

struct reg_state {
    enum origin_t {
        UNKNOWN,
        DATA_OFFSET,   // derived from X0 (data pointer) at a known byte offset
        PC_RELATIVE,   // resolved ADRP+ADD absolute address (safe to read)
        CONST,         // known integer constant (enables branch pruning)
    };
    origin_t origin = UNKNOWN;
    union {
        size_t offset;        // DATA_OFFSET: byte offset into data struct
        const void* address;  // PC_RELATIVE: resolved virtual address
        int64_t const_value;  // CONST: known integer value
    } u = {};
};

// 🎯T3.10: the inbound X0–X7 provenance a callee's analyser starts from.
// Pre-T3.10 this was implicitly { regs[0] = DATA_OFFSET(0) }; forwarding a
// callable that arrives in X1–X7 (or a CONST discriminator) seeds the
// corresponding register instead of leaving it UNKNOWN.
struct callee_seed {
    reg_state regs[8];   // default-constructed: all UNKNOWN
};

// The seed an unspecialised callee walk starts from: the data pointer in X0
// at offset 0 (the AAPCS64 default for a void(*)(void*) entry function).
inline callee_seed default_seed() {
    callee_seed s;
    s.regs[0].origin = reg_state::DATA_OFFSET;
    s.regs[0].u.offset = 0;
    return s;
}

// --- Program cache ---

spinlock g_cache_mu;
ptr_map<std::vector<uint8_t>> g_cache;

// Eval result cache: for data=nullptr evaluations, the result is deterministic.
// Keyed by function address.
spinlock g_eval_cache_mu;
ptr_map<stack_analysis> g_eval_cache;

// Forward declaration.
std::vector<uint8_t> analyze_and_compile(const void* fn,
                                         const stack_analysis_options& opts,
                                         const void* data,
                                         const callee_seed& seed);

// Return by value: programs are typically 9-27 bytes, so copies are cheap.
// A `cacheable` walk (data==nullptr, default seed) reuses the global program
// cache keyed by `fn`. A specialised walk — one carrying a forwarded data
// pointer and/or a non-default register seed (🎯T3.10) — is compiled on
// demand and never touches the cache, because its result depends on call-site
// state, not just the callee address (paper 30 §6.3, Option B).
std::vector<uint8_t> get_or_compile(const void* fn,
                                    const stack_analysis_options& opts,
                                    const void* data,
                                    const callee_seed& seed,
                                    bool cacheable) {
    if (!cacheable) {
        return analyze_and_compile(fn, opts, data, seed);
    }
    {
        std::lock_guard<spinlock> lk(g_cache_mu);
        if (auto* p = g_cache.find(fn)) return *p;
    }
    auto prog = analyze_and_compile(fn, opts, data, seed);
    std::lock_guard<spinlock> lk(g_cache_mu);
    g_cache.emplace(fn, prog);
    return prog;
}

// Convenience overload preserving the pre-T3.10 contract: a data==nullptr
// walk is cacheable; a data-specialised walk is not. Both use the default
// (X0 = DATA_OFFSET(0)) seed.
std::vector<uint8_t> get_or_compile(const void* fn,
                                    const stack_analysis_options& opts,
                                    const void* data = nullptr) {
    return get_or_compile(fn, opts, data, default_seed(),
                          /*cacheable=*/ data == nullptr);
}

// --- Segment bounds (🎯T3.4.2) ---
//
// To safely follow PC-relative function pointer tables and vtable dispatch,
// the walker needs to know:
//   1. Whether an address is in our binary's executable code section
//      (so we can emit OP_CALL_DIRECT pointing at it).
//   2. Whether an address is in our binary's read-only data (so we can
//      dereference it once to load a function pointer).
//
// Anything outside these bounds is treated as "unknown" and falls back to
// the budget — this rejects GOT/PLT stubs, foreign-library targets, and
// arbitrary heap/stack pointers.
//
// macOS: discovered via _dyld_get_image_header(0) + getsectiondata for the
// __TEXT,__text and __DATA_CONST,__const sections. Reading the section
// (rather than the segment) naturally excludes __stubs, which is the
// requirement called out in paper 23 §8 (T3.4.2).
//
// Linux: discovered via dl_iterate_phdr enumerating PT_LOAD segments of
// the main executable. PF_X segments contribute to "text"; non-PF_W
// segments contribute to "readonly". PLT is in its own section and would
// land in a PF_X segment but with a distinct ELF section name; we
// approximate by accepting all PF_X bytes — the secondary check on the
// loaded function pointer (must itself be in PF_X) catches misreads.
//
// Other platforms (Windows, BSDs): both ranges remain empty and the
// PC_RELATIVE → direct-call promotion is skipped, falling back to the
// pre-T3.4.2 budget behaviour.

struct addr_range {
    const char* lo = nullptr;
    const char* hi = nullptr;
    bool contains(const void* p) const {
        auto cp = static_cast<const char*>(p);
        return lo && cp >= lo && cp < hi;
    }
};

struct segment_bounds {
    addr_range text;      // executable code (excluding stubs/PLT where possible)
    addr_range readonly;  // read-only data (vtables, fn-ptr tables, etc.)
};

const segment_bounds& binary_bounds();

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>

const segment_bounds& binary_bounds() {
    static segment_bounds bounds = [] {
        segment_bounds b;
        auto* h = reinterpret_cast<const struct mach_header_64*>(
            _dyld_get_image_header(0));
        if (!h) return b;
        unsigned long sz = 0;
        if (auto* p = getsectiondata(h, "__TEXT", "__text", &sz); p && sz) {
            b.text.lo = reinterpret_cast<const char*>(p);
            b.text.hi = b.text.lo + sz;
        }
        if (auto* p = getsectiondata(h, "__DATA_CONST", "__const", &sz); p && sz) {
            b.readonly.lo = reinterpret_cast<const char*>(p);
            b.readonly.hi = b.readonly.lo + sz;
        }
        return b;
    }();
    return bounds;
}

#elif defined(__linux__)
#include <link.h>

const segment_bounds& binary_bounds() {
    static segment_bounds bounds = [] {
        segment_bounds b;
        dl_iterate_phdr(
            [](struct dl_phdr_info* info, size_t, void* arg) -> int {
                if (info->dlpi_name[0] != '\0') return 0;  // not main exe
                auto& out = *static_cast<segment_bounds*>(arg);
                for (int i = 0; i < info->dlpi_phnum; ++i) {
                    const auto& ph = info->dlpi_phdr[i];
                    if (ph.p_type != PT_LOAD) continue;
                    auto lo = reinterpret_cast<const char*>(
                        info->dlpi_addr + ph.p_vaddr);
                    auto hi = lo + ph.p_memsz;
                    if (ph.p_flags & PF_X) {
                        if (!out.text.lo || lo < out.text.lo) out.text.lo = lo;
                        if (!out.text.hi || hi > out.text.hi) out.text.hi = hi;
                    } else if (!(ph.p_flags & PF_W)) {
                        if (!out.readonly.lo || lo < out.readonly.lo) out.readonly.lo = lo;
                        if (!out.readonly.hi || hi > out.readonly.hi) out.readonly.hi = hi;
                    }
                }
                return 1;  // stop after main exe
            },
            &b);
        return b;
    }();
    return bounds;
}

#else

const segment_bounds& binary_bounds() {
    static segment_bounds empty;
    return empty;
}

#endif

// Read a function pointer from `addr`. The caller is responsible for
// having verified that `addr` is in a safely-dereferenceable region.
inline const void* deref_fnptr(const void* addr) {
    const void* v;
    std::memcpy(&v, addr, sizeof(v));
    return v;
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
//
// reg_state (the per-register origin tag) is defined above, before the
// program cache, so the cache and the callee-entry seeding can reference it.

struct analysis_state {
    const uint32_t* pc;
    int64_t sp_delta;
    reg_state regs[32];
};

// --- Instruction walker (iterative worklist) ---

struct walk_work {
    analysis_state state;
    int branch_depth;
};

// Resolve the callee of an indirect branch (BR Xn tail call) or indirect
// call (BLR Xn) from Xn's tracked provenance (🎯T49: formerly duplicated
// inline at both sites).
//
// `over_budget` forces the budget fallback even for resolvable targets:
// CALL_DIRECT/CALL_INDIRECT expressions make the bytecode evaluator walk
// the callee's body, which is exactly the work the budget cap exists to
// bound. BL, BLR, and B.cond all gated on over_budget; BR historically
// did not — an omission dating to the introduction of the budget in the
// worklist walker (commit 286f4cb), not a design decision. Fixed here:
// an over-budget BR now degrades to the budget constant like every other
// call site (behaviour change: over-budget analyses through indirect
// tail calls get make_budget instead of a resolved callee — more
// conservative, and consistent with the walker's budget contract).
//
// Resolution shapes for PC_RELATIVE (🎯T3.4.2):
//   1. Xn holds a code address (ADRP+ADD on a function symbol — the
//      address itself is in __TEXT,__text).
//   2. Xn holds the address of a function pointer slot (ADRP+ADD on a
//      static-const table, optionally followed by LDR that propagated
//      PC_RELATIVE with a byte offset). The slot is in __DATA_CONST,
//      __const; one dereference yields a code address that should
//      itself land in __TEXT,__text.
// GOT/PLT stubs and foreign-library targets fall outside both ranges
// and route to the budget fallback: dereferencing a GOT entry and
// walking the resulting external symbol risks walking into unmapped
// stubs or system libraries, causing SIGSEGV.
expr_ptr resolve_indirect_callee(
        const analysis_state& state, uint32_t rn,
        const stack_analysis_options& opts, bool over_budget) {
    if (!over_budget && rn < 31) {
        const auto& r = state.regs[rn];
        if (r.origin == reg_state::DATA_OFFSET) {
            return expr::make_call_indirect(r.u.offset);
        }
        if (r.origin == reg_state::PC_RELATIVE) {
            const void* a = r.u.address;
            const auto& bb = binary_bounds();
            if (bb.text.contains(a)) {
                return expr::make_call_direct(a);
            }
            if (bb.readonly.contains(a)) {
                auto* fn = deref_fnptr(a);
                if (bb.text.contains(fn)) {
                    return expr::make_call_direct(fn);
                }
            }
        }
    }
    return expr::make_budget(opts.indirect_call_budget);
}

// 🎯T52.1: closed-world SP-write detector.
//
// The walker models the common SP-adjusting forms inline (SUB/ADD SP #imm,
// pair and single-register writeback on SP). The soundness invariant is
// stronger than that enumeration: ARM64 can architecturally write SP only
// through a bounded set of encoding *classes*, so classifying those classes
// structurally guarantees that any SP-writing instruction the decode loop
// does not model is refused (budget + is_exact=false) instead of silently
// drifting the tracked SP delta — including future encodings inside these
// classes. Anything outside the classes below cannot write SP. Classes:
//   a. Add/subtract (immediate), incl. the MTE tag variants ADDG/SUBG and
//      the MOV to/from SP alias — Rd=SP when not flag-setting.
//   b. Add/subtract (extended register), incl. SUB SP, SP, Xn — Rd=SP when
//      not flag-setting. (The shifted-register forms use XZR, never SP.)
//   c. Load/store pair with pre/post-index writeback — Rn=SP.
//   d. Load/store register (imm9) with pre/post-index writeback — Rn=SP.
//   e. AdvSIMD load/store structures (LD1/ST1 etc.), post-indexed — Rn=SP.
//   f. MTE store-tag family (STG/STZG/ST2G/STZ2G), pre/post-index — Rn=SP.
//   g. IRG Xd|SP, Xn|SP — Rd=SP.
// Classes c and d overlap the modelled writeback handlers; keeping them
// here is deliberate so the detector stands alone as the closed world.
bool writes_sp(uint32_t inst) {
    uint32_t rd = inst & 0x1F;
    uint32_t rn = (inst >> 5) & 0x1F;
    bool flags = (inst & 0x20000000) != 0;  // S bit: register 31 is XZR
    // a. Add/subtract (immediate) / (immediate, with tags): bits[28:24]=10001.
    if (match(inst, 0x1F000000, 0x11000000) && rd == 31 && !flags) return true;
    // b. Add/subtract (extended register): bits[28:21]=01011001.
    if (match(inst, 0x1FE00000, 0x0B200000) && rd == 31 && !flags) return true;
    // c. Load/store pair writeback: bits[29:27]=101, bit25=0, bit23=1.
    if (match(inst, 0x3A800000, 0x28800000) && rn == 31) return true;
    // d. Load/store register imm9 writeback: bits[29:27]=111, bits[25:24]=00,
    //    bit21=0, bit10=1.
    if (match(inst, 0x3B200400, 0x38000400) && rn == 31) return true;
    // e. AdvSIMD load/store structures, post-indexed: bit31=0,
    //    bits[29:25]=00110, bit23=1.
    if (match(inst, 0xBE800000, 0x0C800000) && rn == 31) return true;
    // f. MTE store-tag writeback: bits[31:24]=11011001, bit21=1, bit10=1.
    if (match(inst, 0xFF200400, 0xD9200400) && rn == 31) return true;
    // g. IRG: bits[31:21]=10011010110, bits[15:10]=000100.
    if (match(inst, 0xFFE0FC00, 0x9AC01000) && rd == 31) return true;
    return false;
}

// Walk instructions iteratively, returning one expression per completed path.
// At conditional branches, both paths are pushed to a worklist instead of
// recursing.  BL instructions always emit CALL_DIRECT — callee resolution
// is deferred to the bytecode evaluator, keeping walk() completely
// non-recursive.
//
// When `data` is non-null, integer fields loaded from the data struct via
// LDR Wt/Xt instructions are materialised as CONST register values at walk
// time.  This allows CBZ/CBNZ/TBZ/TBNZ handlers to prune dead branches
// statically, producing tighter depth estimates for data-specialised paths.
std::vector<expr_ptr> walk(
        analysis_state initial_state,
        const stack_analysis_options& opts,
        const void* data,
        visit_set& visited,
        size_t& inst_count) {
    static constexpr int MAX_BRANCH_DEPTH = 64;
    std::vector<expr_ptr> path_results;
    std::vector<walk_work> worklist;
    worklist.push_back({initial_state, 0});

    while (!worklist.empty()) {
        auto work = std::move(worklist.back());
        worklist.pop_back();

        auto state = std::move(work.state);
        int branch_depth = work.branch_depth;
        auto result = expr::make_const(0);
        size_t high_water = 0;

        while (true) {
            // Safety: stop if PC is null or misaligned.
            if (!state.pc || ((uintptr_t)state.pc & 3) != 0) {
                path_results.push_back(expr::make_max(
                    std::move(result), expr::make_const(high_water)));
                break;
            }
            // Track whether we've exceeded our budget. When over budget,
            // we continue the sequential walk (to reach RET/BLR/BR) but
            // stop exploring branch targets and callee bodies.
            bool over_budget = inst_count >= opts.max_instructions ||
                               branch_depth >= MAX_BRANCH_DEPTH;
            inst_count++;

            if (!visited.insert(state.pc, state.sp_delta)) {
                // Back-edge: loop detected, terminate path.
                path_results.push_back(expr::make_max(
                    std::move(result), expr::make_const(high_water)));
                break;
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

            // --- STP/LDP pre/post-indexed writeback on SP (GP and FP/SIMD) ---
            // 🎯T52.1 (future doc §3): the full load/store-pair writeback
            // class, not just the 64-bit GP forms. Class match: bits[29:27]
            // = 101 (load/store pair), bit25 = 0, bit23 = 1 (post-index 001
            // / pre-index 011 in bits[25:23]); Rn = SP makes the base
            // writeback an SP adjustment of sign_extend(imm7) * scale.
            // Scale: V=1 (FP/SIMD) → 4 << opc (S/D/Q pairs); V=0 → 8 for X
            // pairs (opc=10), 4 for W pairs (opc=00) and LDPSW (opc=01
            // loads). The remaining combinations — opc=11 (unallocated) and
            // opc=01 stores (STGP, an MTE store with a ×16 scale) — are not
            // decoded; they fall through to the closed-world SP-write
            // detector below and force the sound inexact posture.
            if (match(inst, 0x3A800000, 0x28800000) &&
                ((inst >> 5) & 0x1F) == 31) {
                uint32_t opc = inst >> 30;
                bool simd = (inst >> 26) & 1;
                bool load = (inst >> 22) & 1;
                if (opc != 3 && (simd || opc != 1 || load)) {
                    int64_t scale =
                        simd ? (int64_t{4} << opc) : (opc == 2 ? 8 : 4);
                    int32_t imm7 = (inst >> 15) & 0x7F;
                    state.sp_delta += sign_extend(imm7, 7) * scale;
                    if (!simd && load) {
                        uint32_t rt = inst & 0x1F;
                        uint32_t rt2 = (inst >> 10) & 0x1F;
                        if (rt < 31) state.regs[rt].origin = reg_state::UNKNOWN;
                        if (rt2 < 31) state.regs[rt2].origin = reg_state::UNKNOWN;
                    }
                    state.pc++;
                    continue;
                }
                // Not decodable: fall through to the SP-write detector.
            }

            // --- STR/LDR (single register) pre/post-indexed writeback on SP ---
            // 🎯T52.1 (future doc §4): load/store register imm9 writeback
            // class. Class match: bits[29:27] = 111, bits[25:24] = 00,
            // bit21 = 0, bit10 = 1 (post-index 01 / pre-index 11 in
            // bits[11:10]; the bit10=0 forms — unscaled LDUR/STUR and
            // unprivileged LDTR/STTR — have no writeback). Covers GP and
            // FP/SIMD single-register forms of every width; imm9 is
            // unscaled for all of them.
            if (match(inst, 0x3B200400, 0x38000400) &&
                ((inst >> 5) & 0x1F) == 31) {
                int32_t imm9 = (inst >> 12) & 0x1FF;
                state.sp_delta += sign_extend(imm9, 9);
                bool simd = (inst >> 26) & 1;
                uint32_t opc = (inst >> 22) & 0x3;
                if (!simd && opc != 0) {  // GP load forms write Rt
                    uint32_t rt = inst & 0x1F;
                    if (rt < 31) state.regs[rt].origin = reg_state::UNKNOWN;
                }
                state.pc++;
                continue;
            }

            // --- RET ---
            if (match(inst, 0xFFFFFC1F, 0xD65F0000)) {
                path_results.push_back(expr::make_max(
                    std::move(result), expr::make_const(high_water)));
                break;
            }

            // --- BR Xn (indirect tail call / indirect branch) ---
            if (match(inst, 0xFFFFFC1F, 0xD61F0000)) {
                uint32_t rn = (inst >> 5) & 0x1F;

                auto callee =
                    resolve_indirect_callee(state, rn, opts, over_budget);

                auto call_expr = expr::make_add(expr::make_const(cur_depth),
                                                std::move(callee));
                path_results.push_back(expr::make_max(
                    std::move(result),
                    expr::make_max(expr::make_const(high_water),
                                   std::move(call_expr))));
                break;
            }

            // --- BL offset ---
            if (match(inst, 0xFC000000, 0x94000000)) {
                int64_t imm26 = sign_extend(inst & 0x3FFFFFF, 26);
                const void* target = state.pc + imm26;

                expr_ptr callee;
                if (over_budget) {
                    callee = expr::make_budget(opts.indirect_call_budget);
                } else {
                    // 🎯T3.10: capture the inbound provenance of argument
                    // registers X0–X7 and forward it to the callee, so a
                    // callable or discriminator arriving in X1–X7 (not just
                    // X0) can be resolved or pruned by the callee's own
                    // analyser. By AAPCS64 the register a caller places an
                    // argument in is the register the callee reads it from,
                    // so seeding regs[i] in the callee mirrors the real
                    // handoff (paper 30 §5.2). Anything the scan can't see
                    // (stack-passed args, spills) is simply not forwarded and
                    // the callee falls back to budget — sound by construction
                    // (paper 30 §5.3, §8).
                    //   - The first DATA_OFFSET register becomes the callee's
                    //     forwarded data pointer (the 🎯T3.4.3 mechanism), now
                    //     remembering which register it lands in.
                    //   - PC_RELATIVE (resolved code / table addresses) and
                    //     CONST (branch discriminators) are self-contained and
                    //     forwarded as absolute seeds.
                    int data_reg = -1;
                    uint64_t data_offset = 0;
                    int n_abs = 0;
                    std::vector<arg_prov> seeds;
                    for (int i = 0; i < 8; ++i) {
                        const auto& r = state.regs[i];
                        switch (r.origin) {
                        case reg_state::DATA_OFFSET:
                            // Only the first (lowest) DATA_OFFSET can be
                            // threaded as the single forwarded data pointer;
                            // dropping any others only widens (sound).
                            if (data_reg < 0) {
                                data_reg = i;
                                data_offset = r.u.offset;
                                seeds.push_back(
                                    {static_cast<uint8_t>(i),
                                     static_cast<uint8_t>(reg_state::DATA_OFFSET),
                                     r.u.offset});
                            }
                            break;
                        case reg_state::PC_RELATIVE:
                            seeds.push_back(
                                {static_cast<uint8_t>(i),
                                 static_cast<uint8_t>(reg_state::PC_RELATIVE),
                                 reinterpret_cast<uint64_t>(r.u.address)});
                            ++n_abs;
                            break;
                        case reg_state::CONST:
                            seeds.push_back(
                                {static_cast<uint8_t>(i),
                                 static_cast<uint8_t>(reg_state::CONST),
                                 static_cast<uint64_t>(r.u.const_value)});
                            ++n_abs;
                            break;
                        case reg_state::UNKNOWN:
                            break;
                        }
                    }
                    if (seeds.empty()) {
                        callee = expr::make_call_direct(target);
                    } else if (n_abs == 0) {
                        // Sole DATA_OFFSET register — keep the compact opcode
                        // (byte-identical to pre-T3.10 when it lands in X0).
                        callee = expr::make_call_direct_with_data(
                            target, pack_data_prov(data_reg, data_offset));
                    } else {
                        callee = expr::make_call_direct_with_args(
                            target, std::move(seeds));
                    }
                }

                auto call_expr = expr::make_add(expr::make_const(cur_depth),
                                                std::move(callee));
                result = expr::make_max(std::move(result), std::move(call_expr));

                // After a BL, the callee may clobber X0-X7 (caller-saved
                // argument/result registers per AAPCS64). Mark them unknown so
                // subsequent loads don't misuse stale provenance.
                for (int i = 0; i < 8; ++i) {
                    state.regs[i].origin = reg_state::UNKNOWN;
                }

                state.pc++;
                continue;
            }

            // --- BLR Xn ---
            if (match(inst, 0xFFFFFC1F, 0xD63F0000)) {
                uint32_t rn = (inst >> 5) & 0x1F;

                auto callee =
                    resolve_indirect_callee(state, rn, opts, over_budget);

                auto call_expr = expr::make_add(expr::make_const(cur_depth),
                                                std::move(callee));
                result = expr::make_max(std::move(result), std::move(call_expr));

                // BLR clobbers X0-X7 (caller-saved).
                for (int i = 0; i < 8; ++i) {
                    state.regs[i].origin = reg_state::UNKNOWN;
                }

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
                    result = expr::make_max(std::move(result),
                                            expr::make_budget(opts.indirect_call_budget));
                    state.pc++;
                    continue;
                }
                int64_t imm19 = sign_extend((inst >> 5) & 0x7FFFF, 19);
                const uint32_t* target = state.pc + imm19;

                // Emit pre-branch BL call expressions.
                path_results.push_back(std::move(result));

                // Push both branch paths to worklist.
                analysis_state taken_state = state;
                taken_state.pc = target;
                worklist.push_back({std::move(taken_state), branch_depth + 1});

                state.pc++;
                worklist.push_back({std::move(state), branch_depth + 1});
                break;
            }

            // --- CBZ / CBNZ ---
            if (match(inst, 0x7F000000, 0x34000000) ||
                match(inst, 0x7F000000, 0x35000000)) {
                if (over_budget) {
                    result = expr::make_max(std::move(result),
                                            expr::make_budget(opts.indirect_call_budget));
                    state.pc++;
                    continue;
                }
                bool is_cbnz = (inst & 0x01000000) != 0;
                uint32_t rn = inst & 0x1F;
                int64_t imm19 = sign_extend((inst >> 5) & 0x7FFFF, 19);
                const uint32_t* target = state.pc + imm19;

                // Condition evaluation: if rn has a known constant value,
                // follow only the branch that would be taken.
                if (rn < 31 && state.regs[rn].origin == reg_state::CONST) {
                    bool taken = is_cbnz
                        ? (state.regs[rn].u.const_value != 0)
                        : (state.regs[rn].u.const_value == 0);
                    state.pc = taken ? target : state.pc + 1;
                    continue;
                }

                path_results.push_back(std::move(result));

                analysis_state taken_state = state;
                taken_state.pc = target;
                worklist.push_back({std::move(taken_state), branch_depth + 1});

                state.pc++;
                worklist.push_back({std::move(state), branch_depth + 1});
                break;
            }

            // --- TBZ / TBNZ ---
            if (match(inst, 0x7F000000, 0x36000000) ||
                match(inst, 0x7F000000, 0x37000000)) {
                if (over_budget) {
                    result = expr::make_max(std::move(result),
                                            expr::make_budget(opts.indirect_call_budget));
                    state.pc++;
                    continue;
                }
                bool is_tbnz = (inst & 0x01000000) != 0;
                uint32_t rn = inst & 0x1F;
                uint32_t bit_pos = (inst >> 19) & 0x3F;  // b5:b40
                int64_t imm14 = sign_extend((inst >> 5) & 0x3FFF, 14);
                const uint32_t* target = state.pc + imm14;

                // Condition evaluation with known constants.
                if (rn < 31 && state.regs[rn].origin == reg_state::CONST) {
                    int64_t bit = (state.regs[rn].u.const_value >> bit_pos) & 1;
                    bool taken = is_tbnz ? (bit != 0) : (bit == 0);
                    state.pc = taken ? target : state.pc + 1;
                    continue;
                }

                path_results.push_back(std::move(result));

                analysis_state taken_state = state;
                taken_state.pc = target;
                worklist.push_back({std::move(taken_state), branch_depth + 1});

                state.pc++;
                worklist.push_back({std::move(state), branch_depth + 1});
                break;
            }

            // (SUB SP, SP, Xn — register-based dynamic stack adjustment —
            // assembles to the add/sub extended-register class with Rd=SP
            // and is handled by the closed-world SP-write detector below:
            // immediate budget path, is_exact=false.)

            // --- Register tracking: LDR Xt, [Xn, #imm] (64-bit, unsigned offset) ---
            if (match(inst, 0xFFC00000, 0xF9400000)) {
                uint32_t rt = inst & 0x1F;
                uint32_t rn = (inst >> 5) & 0x1F;
                uint32_t imm12 = (inst >> 10) & 0xFFF;
                size_t byte_offset = imm12 * 8; // scaled by 8 for 64-bit LDR

                if (rt < 31) {
                    if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                        size_t abs_offset = state.regs[rn].u.offset + byte_offset;
                        // 🎯T3.4.2: when data is provided, try reading the
                        // 64-bit value through the closure. If it lands in
                        // our binary's text or read-only data, promote to
                        // PC_RELATIVE — this unlocks vtable dispatch
                        // (closure → vptr → method) and direct-via-closure
                        // function-pointer fields. Otherwise fall through to
                        // the legacy DATA_OFFSET propagation, which keeps the
                        // OP_CALL_INDIRECT path intact for plain function-
                        // pointer-in-closure dispatch.
                        if (data) {
                            const void* v;
                            std::memcpy(&v,
                                static_cast<const char*>(data) + abs_offset,
                                sizeof(v));
                            const auto& bb = binary_bounds();
                            if (bb.text.contains(v) || bb.readonly.contains(v)) {
                                state.regs[rt].origin = reg_state::PC_RELATIVE;
                                state.regs[rt].u.address = v;
                                state.pc++;
                                continue;
                            }
                        }
                        // Load from data struct: new DATA_OFFSET pointing into it.
                        state.regs[rt].origin = reg_state::DATA_OFFSET;
                        state.regs[rt].u.offset = abs_offset;
                    } else if (rn < 31 &&
                               state.regs[rn].origin == reg_state::PC_RELATIVE) {
                        // Load from a PC-relative address: produce a new PC_RELATIVE
                        // register holding the (base + offset) address, not the value
                        // at that address.  The actual function pointer dereference
                        // happens lazily in the BLR handler where we know it is safe
                        // (the register is used as a call target).  Doing a speculative
                        // dereference here can fault if the ADRP target is a GOT entry
                        // pointing to a PLT stub or other indirection structure.
                        const char* base = static_cast<const char*>(
                            state.regs[rn].u.address);
                        state.regs[rt].origin = reg_state::PC_RELATIVE;
                        state.regs[rt].u.address = base + byte_offset;
                    } else {
                        // LDR from SP or unknown register — not data-derived.
                        state.regs[rt].origin = reg_state::UNKNOWN;
                    }
                }
                state.pc++;
                continue;
            }

            // --- Register tracking: LDR Wt, [Xn, #imm] (32-bit, unsigned offset) ---
            // Used for loading integer fields (tags, modes, counts) from closures.
            if (match(inst, 0xFFC00000, 0xB9400000)) {
                uint32_t rt = inst & 0x1F;
                uint32_t rn = (inst >> 5) & 0x1F;
                uint32_t imm12 = (inst >> 10) & 0xFFF;
                size_t byte_offset = imm12 * 4; // scaled by 4 for 32-bit LDR

                if (rt < 31) {
                    if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                        size_t abs_offset = state.regs[rn].u.offset + byte_offset;
                        if (data) {
                            // Materialise the field as a CONST so CBZ/CBNZ/TBZ/TBNZ
                            // can prune dead branches at walk time.
                            uint32_t val;
                            std::memcpy(&val,
                                static_cast<const char*>(data) + abs_offset,
                                sizeof(val));
                            state.regs[rt].origin = reg_state::CONST;
                            state.regs[rt].u.const_value = static_cast<int64_t>(val);
                        } else {
                            // Without data, record provenance for the bytecode evaluator.
                            state.regs[rt].origin = reg_state::DATA_OFFSET;
                            state.regs[rt].u.offset = abs_offset;
                        }
                    } else {
                        state.regs[rt].origin = reg_state::UNKNOWN;
                    }
                }
                state.pc++;
                continue;
            }

            // --- Register tracking: LDRB Wt, [Xn, #imm] (8-bit, unsigned offset) ---
            // 🎯T3.4.3: parity with the 32-bit LDR path for byte-width
            // discriminators (uint8_t tags, bools). Without this, a closure
            // field loaded as a byte was silently widened to MAX.
            if (match(inst, 0xFFC00000, 0x39400000)) {
                uint32_t rt = inst & 0x1F;
                uint32_t rn = (inst >> 5) & 0x1F;
                uint32_t imm12 = (inst >> 10) & 0xFFF;
                size_t byte_offset = imm12; // unscaled for 8-bit LDR

                if (rt < 31) {
                    if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                        size_t abs_offset = state.regs[rn].u.offset + byte_offset;
                        if (data) {
                            uint8_t val;
                            std::memcpy(&val,
                                static_cast<const char*>(data) + abs_offset,
                                sizeof(val));
                            state.regs[rt].origin = reg_state::CONST;
                            state.regs[rt].u.const_value = static_cast<int64_t>(val);
                        } else {
                            state.regs[rt].origin = reg_state::DATA_OFFSET;
                            state.regs[rt].u.offset = abs_offset;
                        }
                    } else {
                        state.regs[rt].origin = reg_state::UNKNOWN;
                    }
                }
                state.pc++;
                continue;
            }

            // --- Register tracking: LDRH Wt, [Xn, #imm] (16-bit, unsigned offset) ---
            // 🎯T3.4.3: parity for half-word discriminators (uint16_t,
            // small enum tags).
            if (match(inst, 0xFFC00000, 0x79400000)) {
                uint32_t rt = inst & 0x1F;
                uint32_t rn = (inst >> 5) & 0x1F;
                uint32_t imm12 = (inst >> 10) & 0xFFF;
                size_t byte_offset = imm12 * 2; // scaled by 2 for 16-bit LDR

                if (rt < 31) {
                    if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                        size_t abs_offset = state.regs[rn].u.offset + byte_offset;
                        if (data) {
                            uint16_t val;
                            std::memcpy(&val,
                                static_cast<const char*>(data) + abs_offset,
                                sizeof(val));
                            state.regs[rt].origin = reg_state::CONST;
                            state.regs[rt].u.const_value = static_cast<int64_t>(val);
                        } else {
                            state.regs[rt].origin = reg_state::DATA_OFFSET;
                            state.regs[rt].u.offset = abs_offset;
                        }
                    } else {
                        state.regs[rt].origin = reg_state::UNKNOWN;
                    }
                }
                state.pc++;
                continue;
            }

            // --- Register tracking: ADD Xd, Xn, #imm ---
            // Note: must check AFTER ADD SP,SP,#imm (which has Rd=Rn=SP).
            // Rd=SP with Rn!=SP (the MOV SP, Xn alias family) is an
            // unmodelled SP write — excluded here so it reaches the
            // closed-world SP-write detector below (🎯T52.1).
            if (match(inst, 0xFF000000, 0x91000000) && (inst & 0x1F) != 31) {
                uint32_t rd = inst & 0x1F;
                uint32_t rn = (inst >> 5) & 0x1F;
                uint32_t imm12 = (inst >> 10) & 0xFFF;
                uint32_t shift = (inst >> 22) & 0x3;
                size_t imm = imm12;
                if (shift == 1) imm <<= 12;

                if (rd < 31) {
                    if (rn < 31 && state.regs[rn].origin == reg_state::DATA_OFFSET) {
                        state.regs[rd].origin = reg_state::DATA_OFFSET;
                        state.regs[rd].u.offset = state.regs[rn].u.offset + imm;
                    } else if (rn < 31 &&
                               state.regs[rn].origin == reg_state::PC_RELATIVE) {
                        // ADD Xd, ADRP_reg, #pageoff — second half of ADRP+ADD
                        // to form a symbol address. Refine the page address.
                        const char* base = static_cast<const char*>(
                            state.regs[rn].u.address);
                        state.regs[rd].origin = reg_state::PC_RELATIVE;
                        state.regs[rd].u.address = base + imm;
                    } else {
                        state.regs[rd].origin = reg_state::UNKNOWN;
                    }
                }
                state.pc++;
                continue;
            }

            // --- Register tracking: ADRP Xd, label ---
            // Forms the page-aligned PC-relative address; subsequent ADD refines it.
            if (match(inst, 0x9F000000, 0x90000000)) {
                uint32_t rd = inst & 0x1F;
                // immhi:immlo = [23:5] cat [30:29], scaled by 4096.
                uint32_t immlo = (inst >> 29) & 0x3;
                uint32_t immhi = (inst >> 5) & 0x7FFFF;
                int64_t imm = sign_extend((immhi << 2) | immlo, 21) << 12;
                // PC page = PC aligned down to 4KB.
                uintptr_t pc_page =
                    reinterpret_cast<uintptr_t>(state.pc) & ~uintptr_t{0xFFF};
                if (rd < 31) {
                    state.regs[rd].origin = reg_state::PC_RELATIVE;
                    state.regs[rd].u.address =
                        reinterpret_cast<const void*>(
                            static_cast<uintptr_t>(
                                static_cast<int64_t>(pc_page) + imm));
                }
                state.pc++;
                continue;
            }

            // --- Register tracking: MOVZ Xd, #imm, LSL #shift ---
            // MOVZ: opc=10, sets register to zero-extended immediate.
            if (match(inst, 0x7F800000, 0x52800000) || // 32-bit MOVZ
                match(inst, 0x7F800000, 0xD2800000)) { // 64-bit MOVZ
                uint32_t rd = inst & 0x1F;
                uint32_t imm16 = (inst >> 5) & 0xFFFF;
                uint32_t hw = (inst >> 21) & 0x3;
                if (rd < 31) {
                    state.regs[rd].origin = reg_state::CONST;
                    state.regs[rd].u.const_value =
                        static_cast<int64_t>(static_cast<uint64_t>(imm16) << (hw * 16));
                }
                state.pc++;
                continue;
            }

            // --- Register tracking: MOVK Xd, #imm, LSL #shift ---
            // MOVK: keep other bits, insert 16-bit field.
            if (match(inst, 0x7F800000, 0x72800000) || // 32-bit MOVK
                match(inst, 0x7F800000, 0xF2800000)) { // 64-bit MOVK
                uint32_t rd = inst & 0x1F;
                uint32_t imm16 = (inst >> 5) & 0xFFFF;
                uint32_t hw = (inst >> 21) & 0x3;
                if (rd < 31 && state.regs[rd].origin == reg_state::CONST) {
                    // Update the relevant 16-bit field.
                    uint64_t mask = ~(static_cast<uint64_t>(0xFFFF) << (hw * 16));
                    uint64_t prev = static_cast<uint64_t>(
                        state.regs[rd].u.const_value);
                    state.regs[rd].u.const_value = static_cast<int64_t>(
                        (prev & mask) | (static_cast<uint64_t>(imm16) << (hw * 16)));
                } else if (rd < 31) {
                    state.regs[rd].origin = reg_state::UNKNOWN;
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

            // --- Closed-world SP-write refusal (🎯T52.1) ---
            // Any instruction that can write SP and was not decoded above
            // leaves the tracked SP delta meaningless from here on. Sound
            // posture: terminate the path with the depth reached so far
            // plus the indirect-call budget (covering plausible further
            // growth); the BUDGET term clears is_exact at eval time.
            if (writes_sp(inst)) {
                path_results.push_back(expr::make_max(
                    std::move(result),
                    expr::make_max(
                        expr::make_const(high_water),
                        expr::make_add(
                            expr::make_const(cur_depth),
                            expr::make_budget(opts.indirect_call_budget)))));
                break;
            }

            // --- Any other instruction: check if it writes a GP register ---
            // Conservative: mark destination register as unknown for common
            // instruction formats that write to Rd at bits [4:0].
            {
                uint32_t rd = inst & 0x1F;
                if (rd < 31) {
                    state.regs[rd].origin = reg_state::UNKNOWN;
                }
            }

            state.pc++;
        }
    }

    return path_results;
}

// Analyze a single function and compile to bytecode.
// walk() does not follow calls — it emits CALL_DIRECT for BL instructions,
// deferring callee resolution to the bytecode evaluator.
//
// When `data` is non-null, integer fields are read from the live closure at
// walk time, allowing branches conditioned on closure data to be pruned
// statically.  Programs compiled with data are not stored in the global
// cache (they are data-specific).
std::vector<uint8_t> analyze_and_compile(const void* fn,
                                         const stack_analysis_options& opts,
                                         const void* data,
                                         const callee_seed& seed) {
    analysis_state initial{};
    initial.pc = static_cast<const uint32_t*>(fn);
    initial.sp_delta = 0;
    // 🎯T3.10: seed the callee's inbound argument registers X0–X7. The
    // default seed is { X0 = DATA_OFFSET(0) } (the AAPCS64 void(*)(void*)
    // entry shape); a forwarded call seeds the register the callable /
    // discriminator actually arrives in.
    for (int i = 0; i < 8; ++i) initial.regs[i] = seed.regs[i];

    visit_set visited;
    size_t inst_count = 0;
    auto results = walk(initial, opts, data, visited, inst_count);

    // Constant-fold pure path results.
    for (auto& r : results) {
        if (is_pure(*r)) {
            r = expr::make_const(eval_pure(*r));
        }
    }

    // Compile each path result and combine with OP_MAX.
    std::vector<uint8_t> prog;
    if (results.empty()) {
        prog.push_back(OP_PUSH);
        emit_u64(prog, 0);
    } else {
        compile(*results[0], prog);
        for (size_t i = 1; i < results.size(); i++) {
            compile(*results[i], prog);
            prog.push_back(OP_MAX);
        }
    }
    return prog;
}

// --- Iterative bytecode evaluator ---
//
// Evaluates a function's compiled bytecode using an explicit call stack
// instead of C++ recursion.  When a CALL_DIRECT or CALL_INDIRECT opcode
// is encountered, the callee is compiled on demand (via get_or_compile,
// which itself is non-recursive) and entered by pushing the current
// instruction pointer onto the call stack.  This keeps the C++ stack
// at constant depth regardless of the analysed program's call chain.

static constexpr int EVAL_STACK_SIZE = 256;

stack_analysis eval_iterative(const void* root_fn, const void* data,
                               const stack_analysis_options& opts) {
    // Check eval cache for the root (data=nullptr only).
    if (!data) {
        std::lock_guard<spinlock> lk(g_eval_cache_mu);
        if (auto* p = g_eval_cache.find(root_fn)) return *p;
    }

    // Evaluation state.
    size_t values[EVAL_STACK_SIZE];
    int vsp = 0;
    bool is_exact = true;

    struct eval_frame {
        const uint8_t* return_ip;
        const uint8_t* return_end;
        const void* fn;
        bool is_exact_before;
        const void* saved_data;
        bool cacheable;          // 🎯T3.10: store result keyed by fn only if true
    };
    std::vector<eval_frame> call_stack;
    // Cycle detection. 🎯T52.1: capacity = opts.max_call_depth (clamped to
    // the set's compile-time cap of 64), so the option is the wired
    // call-following depth limit.
    small_ptr_set on_stack(opts.max_call_depth);

    // Program storage: deque provides stable references on push_back,
    // so ip/end pointers into stored programs remain valid when new
    // programs are added for callees.
    //
    // The root function is compiled with data when available so that
    // integer fields in the closure can be materialised as CONST registers
    // at walk time, enabling branch pruning (e.g. CBZ on a tag field).
    std::deque<std::vector<uint8_t>> prog_store;
    prog_store.push_back(get_or_compile(root_fn, opts, data));

    const uint8_t* ip = prog_store.back().data();
    const uint8_t* end = ip + prog_store.back().size();
    // The root always fits: the on-stack limit is clamped to >= 1.
    (void)on_stack.insert(root_fn);
    const void* current_data = data;

    // 🎯T3.10: enter callee `addr` with forwarded data `fwd_data` and inbound
    // register `seed`. `cacheable` selects whether the address-keyed eval
    // cache may serve and store this callee's result — only unspecialised
    // walks (default seed, no forwarded data) are cacheable (Option B, paper
    // 30 §6.3). On a cache hit or detected recursion the result is pushed and
    // the callee is not entered.
    auto enter_callee = [&](const void* addr, const void* fwd_data,
                            const callee_seed& seed, bool cacheable) {
        if (cacheable) {
            std::lock_guard<spinlock> lk(g_eval_cache_mu);
            if (auto* r = g_eval_cache.find(addr)) {
                is_exact &= r->is_exact;
                values[vsp++] = r->max_depth;
                return;
            }
        }
        if (on_stack.contains(addr)) {
            is_exact = false;
            values[vsp++] = opts.indirect_call_budget;
            return;
        }
        // Never walk a target that isn't in our executable text. An indirect
        // target resolved from data (OP_CALL_INDIRECT, or a register seed)
        // can be a non-function pointer when the data isn't a real fn-pointer
        // table — e.g. a chained pointer load whose DATA_OFFSET base the
        // single-base model can't follow. Walking it would fault; budget is
        // the sound fallback. Direct (BL-derived) targets are always in text,
        // so this never costs exactness for them.
        if (!binary_bounds().text.contains(addr)) {
            is_exact = false;
            values[vsp++] = opts.indirect_call_budget;
            return;
        }
        // 🎯T52.1: on-stack set at capacity — the call chain reached the
        // wired max_call_depth (hard cap 64). Beyond this point cycle
        // detection could no longer be trusted (a cycle among un-inserted
        // functions would evade contains()), so treat exactly like a
        // detected cycle: budget, clear exactness, do not enter.
        if (!on_stack.insert(addr)) {
            is_exact = false;
            values[vsp++] = opts.indirect_call_budget;
            return;
        }
        prog_store.push_back(get_or_compile(addr, opts, fwd_data, seed, cacheable));
        call_stack.push_back({ip, end, addr, is_exact, current_data, cacheable});
        is_exact = true;
        current_data = fwd_data;
        ip = prog_store.back().data();
        end = ip + prog_store.back().size();
    };

    while (true) {
        if (ip >= end) {
            if (call_stack.empty()) break;
            // Return from callee — cache its result only for unspecialised
            // (cacheable) walks; a result specialised to forwarded data or a
            // non-default register seed must not be served keyed by fn alone.
            auto& frame = call_stack.back();
            bool callee_exact = is_exact;
            size_t callee_depth = vsp > 0 ? values[vsp - 1] : 0;
            if (frame.cacheable) {
                std::lock_guard<spinlock> lk(g_eval_cache_mu);
                g_eval_cache.emplace(frame.fn,
                    stack_analysis{callee_depth, callee_exact});
            }
            // Restore caller state.
            is_exact = frame.is_exact_before & callee_exact;
            current_data = frame.saved_data;
            on_stack.erase(frame.fn);
            ip = frame.return_ip;
            end = frame.return_end;
            call_stack.pop_back();
            continue;
        }

        if (vsp >= EVAL_STACK_SIZE - 1) {
            is_exact = false;
            break;
        }

        switch (*ip++) {
        case OP_PUSH:
            values[vsp++] = read_u64(ip);
            break;
        case OP_MAX: {
            if (vsp < 2) break;
            auto b = values[--vsp], a = values[--vsp];
            values[vsp++] = std::max(a, b);
            break;
        }
        case OP_ADD: {
            if (vsp < 2) break;
            auto b = values[--vsp], a = values[--vsp];
            values[vsp++] = a + b;
            break;
        }
        case OP_CALL_DIRECT: {
            auto addr = read_ptr(ip);
            enter_callee(addr, nullptr, default_seed(), /*cacheable=*/true);
            break;
        }
        case OP_CALL_INDIRECT: {
            auto off = read_u64(ip);
            if (!current_data) {
                is_exact = false;
                values[vsp++] = opts.indirect_call_budget;
            } else {
                auto target = *reinterpret_cast<const void* const*>(
                    static_cast<const char*>(current_data) + off);
                enter_callee(target, nullptr, default_seed(), /*cacheable=*/true);
            }
            break;
        }
        case OP_BUDGET: {
            // 🎯T3.4.4: the u64 baked at walk time is consumed but
            // discarded — we use opts.indirect_call_budget at eval time so
            // callers (notably spawn() with profile-derived budgets) can
            // refine the magnitude without invalidating the cached program.
            (void)read_u64(ip);
            is_exact = false;
            values[vsp++] = opts.indirect_call_budget;
            break;
        }
        case OP_CALL_DIRECT_WITH_DATA: {
            // Direct call forwarding a data sub-pointer to the callee. The
            // callee receives *(current_data + data_off) as its data arg, in
            // the argument register the parent forwarded it from (🎯T3.10).
            auto addr = read_ptr(ip);
            auto raw = read_u64(ip);
            int data_reg = unpack_data_reg(raw);
            auto data_off = unpack_data_off(raw);
            // We only ever pack reg indices 0–7; guard the fixed-size seed.
            if (data_reg >= 8) data_reg = 0;

            // Resolve the forwarded data pointer (pointer within the closure).
            const void* forwarded_data = nullptr;
            if (current_data) {
                forwarded_data = *reinterpret_cast<const void* const*>(
                    static_cast<const char*>(current_data) + data_off);
            }

            // The callee sees its data at offset 0 of the forwarded pointer,
            // in register `data_reg`.
            callee_seed seed;
            seed.regs[data_reg].origin = reg_state::DATA_OFFSET;
            seed.regs[data_reg].u.offset = 0;

            // Cacheable only when this reduces to the unspecialised default:
            // no forwarded data and the data pointer in X0 (paper 30 §6.3).
            bool cacheable = (forwarded_data == nullptr && data_reg == 0);
            enter_callee(addr, forwarded_data, seed, cacheable);
            break;
        }
        case OP_CALL_DIRECT_WITH_ARGS: {
            // 🎯T3.10: direct call forwarding the inbound provenance of
            // several argument registers. Seeds the callee's X0–X7 so a
            // callable arriving in X1–X7 resolves and a forwarded CONST
            // discriminator prunes — always a call-site-specialised (and
            // therefore uncached) walk.
            auto addr = read_ptr(ip);
            uint8_t count = *ip++;
            callee_seed seed;
            const void* forwarded_data = nullptr;
            for (uint8_t k = 0; k < count; ++k) {
                uint8_t reg = *ip++;
                uint8_t origin = *ip++;
                uint64_t payload = read_u64(ip);
                if (reg >= 8) continue;  // defensive: only 0–7 are emitted
                switch (static_cast<reg_state::origin_t>(origin)) {
                case reg_state::DATA_OFFSET:
                    // The data pointer arrives in `reg`; dereference the
                    // caller's data to get the callee's data sub-pointer.
                    if (current_data) {
                        forwarded_data = *reinterpret_cast<const void* const*>(
                            static_cast<const char*>(current_data) + payload);
                    }
                    seed.regs[reg].origin = reg_state::DATA_OFFSET;
                    seed.regs[reg].u.offset = 0;
                    break;
                case reg_state::PC_RELATIVE:
                    seed.regs[reg].origin = reg_state::PC_RELATIVE;
                    seed.regs[reg].u.address =
                        reinterpret_cast<const void*>(payload);
                    break;
                case reg_state::CONST:
                    seed.regs[reg].origin = reg_state::CONST;
                    seed.regs[reg].u.const_value = static_cast<int64_t>(payload);
                    break;
                case reg_state::UNKNOWN:
                    break;
                }
            }
            enter_callee(addr, forwarded_data, seed, /*cacheable=*/false);
            break;
        }
        }
    }

    stack_analysis result = {vsp > 0 ? values[0] : 0, is_exact};

    // Cache root result for data=nullptr.
    if (!data) {
        std::lock_guard<spinlock> lk(g_eval_cache_mu);
        g_eval_cache.emplace(root_fn, result);
    }

    return result;
}

} // anonymous namespace

stack_analysis analyze_stack_depth(const void* fn, const void* data,
                                  stack_analysis_options opts) {
    return eval_iterative(fn, data, opts);
}

stack_analysis analyze_stack_depth_cached(const void* fn, const void* data,
                                          stack_analysis_options opts) {
    // Check eval cache only — no recursive analysis.
    if (!data) {
        std::lock_guard<spinlock> lk(g_eval_cache_mu);
        if (auto* p = g_eval_cache.find(fn)) return *p;
    }
    // Not cached — return conservative default.
    return {32u << 10, false};
}

// --- 🎯T52.4: async analysis (Default-on-miss + worker thread) ---
//
// Tiered-JIT shape, minus deoptimization (Default is unconditionally
// safe): the spawn path consults the fn-keyed result cache through the
// allocation-free stub below; on a miss it takes the Default slot
// immediately and enqueues the entry on a fixed-capacity MPSC ring. A
// dedicated runtime-owned OS thread (NOT an imp — a real full-size
// stack) dequeues, runs the ordinary synchronous analysis, and publishes
// through the existing spinlocked caches. First-spawn-per-entry misses
// Small; every later spawn of a published exact-and-fitting entry hits.
//
// Only the stub executes on imp stacks, so only the stub must remain
// allocation-free and walkable. The worker (and everything it calls) may
// use ordinary containers.

namespace {

// Fixed-capacity MPSC request ring (Vyukov bounded-queue cells). Static
// storage, no allocation on either side; try-push fails when full (the
// request is dropped — a later spawn of the same entry retries).
constexpr size_t kAnalysisQueueCap = 256;  // power of two

struct analysis_queue_cell {
    std::atomic<size_t> seq;
    const void* fn;
};

analysis_queue_cell g_analysis_queue[kAnalysisQueueCap];
std::atomic<size_t> g_analysis_enqueue_pos{0};
std::atomic<size_t> g_analysis_dequeue_pos{0};

// One-time sequence-number init (runs during static initialization,
// strictly before any spawn can enqueue).
struct analysis_queue_init {
    analysis_queue_init() {
        for (size_t i = 0; i < kAnalysisQueueCap; ++i) {
            g_analysis_queue[i].seq.store(i, std::memory_order_relaxed);
        }
    }
} g_analysis_queue_init;

// Multi-producer push. Pure inline atomics — this is part of the spawn
// stub's walk corpus, so it must contain no BL to malloc or any other
// unwalkable callee.
bool analysis_queue_push(const void* fn) noexcept {
    size_t pos = g_analysis_enqueue_pos.load(std::memory_order_relaxed);
    for (;;) {
        auto& cell = g_analysis_queue[pos & (kAnalysisQueueCap - 1)];
        size_t seq = cell.seq.load(std::memory_order_acquire);
        auto dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
        if (dif == 0) {
            if (g_analysis_enqueue_pos.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                cell.fn = fn;
                cell.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;  // full
        } else {
            pos = g_analysis_enqueue_pos.load(std::memory_order_relaxed);
        }
    }
}

// Single-consumer pop (worker thread only).
bool analysis_queue_pop(const void*& fn) noexcept {
    size_t pos = g_analysis_dequeue_pos.load(std::memory_order_relaxed);
    for (;;) {
        auto& cell = g_analysis_queue[pos & (kAnalysisQueueCap - 1)];
        size_t seq = cell.seq.load(std::memory_order_acquire);
        auto dif = static_cast<intptr_t>(seq) -
                   static_cast<intptr_t>(pos + 1);
        if (dif == 0) {
            if (g_analysis_dequeue_pos.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                fn = cell.fn;
                cell.seq.store(pos + kAnalysisQueueCap,
                               std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;  // empty
        } else {
            pos = g_analysis_dequeue_pos.load(std::memory_order_relaxed);
        }
    }
}

bool analysis_queue_empty() noexcept {
    return g_analysis_dequeue_pos.load(std::memory_order_seq_cst) ==
           g_analysis_enqueue_pos.load(std::memory_order_seq_cst);
}

// Worker state. Heap-allocated once and deliberately leaked: the worker
// is stopped by Runtime::shutdown()/~Runtime(), whose static-destruction
// order relative to this TU's globals is unspecified — a static
// std::thread/condition_variable here could be destroyed while the
// worker still runs (std::terminate / UB). A function-local static
// pointer keeps the state reachable (no LeakSanitizer report) and
// trivially immortal.
struct analysis_worker_state {
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false;
    bool running = false;
    std::thread thr;
};

analysis_worker_state& worker_state() {
    static auto* s = new analysis_worker_state();
    return *s;
}

// True between a dequeue and the publication of that request's result,
// so analysis_quiesce() can distinguish "queue empty" from "drained and
// published".
std::atomic<bool> g_analysis_in_flight{false};

void analyse_and_publish(const void* fn) {
    {
        std::lock_guard<spinlock> lk(g_eval_cache_mu);
        if (g_eval_cache.find(fn)) return;  // already published
    }
    stack_analysis_options opts;
    // 🎯T3.4.4 profile feedback, applied worker-side: a recorded
    // high-water (+50% margin) replaces the flat default budget for
    // unresolved indirect edges. It only sharpens the magnitude of
    // inexact results; is_exact remains the sole Small gate.
    if (size_t hw = ::csp::detail::get_stack_high_water(
            reinterpret_cast<::csp::internal::EntryFn>(
                const_cast<void*>(fn)));
        hw > 0) {
        size_t refined = hw + hw / 2;
        if (refined > opts.indirect_call_budget) {
            opts.indirect_call_budget = refined;
        }
    }
    // eval_iterative publishes the fn-keyed root result into g_eval_cache
    // on completion (data == nullptr path).
    (void)analyze_stack_depth(fn, nullptr, opts);
}

void analysis_worker_main() {
    auto& ws = worker_state();
    for (;;) {
        // Drain. in_flight is raised before each pop so quiesce() never
        // observes {queue empty, not in flight} between a dequeue and its
        // publication.
        for (;;) {
            g_analysis_in_flight.store(true, std::memory_order_seq_cst);
            const void* fn = nullptr;
            if (!analysis_queue_pop(fn)) {
                g_analysis_in_flight.store(false, std::memory_order_seq_cst);
                break;
            }
            analyse_and_publish(fn);
            g_analysis_in_flight.store(false, std::memory_order_seq_cst);
        }
        // Park. Timed wait instead of producer-side notification: the
        // spawn stub must stay walkable (a condvar/futex wake is a BL
        // into libc++/libc, which would force the stub inexact), so
        // producers never signal — the worker polls. 10 ms of publish
        // latency is irrelevant to the density story (first-spawn misses
        // Small by design), and an idle condvar timeout costs ~nothing.
        std::unique_lock<std::mutex> lk(ws.mu);
        if (ws.stop) return;
        ws.cv.wait_for(lk, std::chrono::milliseconds(10));
        if (ws.stop) return;
    }
}

} // anonymous namespace

namespace detail {

stack_analysis stack_analysis_lookup_or_request(const void* fn) noexcept {
    // Fn-keyed lookup. Every published entry is an unspecialised
    // (data == nullptr) walk, which is a sound upper bound for ANY spawn
    // data: an exact null-data result means no path depended on data
    // (data-dependent calls budget to inexact without data), and data-
    // derived CONST pruning only ever removes paths from the max.
    {
        std::lock_guard<spinlock> lk(g_eval_cache_mu);
        if (auto* p = g_eval_cache.find(fn)) return *p;
    }
    // Miss: request async analysis (dropped when the ring is full — a
    // later spawn retries) and stand on the Default-slot sentinel.
    analysis_queue_push(fn);
    return {32u << 10, false};
}

void start_stack_analysis_worker() {
    auto& ws = worker_state();
    std::lock_guard<std::mutex> lk(ws.mu);
    if (ws.running) return;
    // Stop the worker at exit BEFORE any static destructor can run. The
    // worker touches this TU's static caches (g_cache / g_eval_cache) and
    // csp.cc's profile table; their destruction order relative to
    // runtime.cpp's g_runtime (whose ~Runtime also stops the worker) is
    // unspecified, and a worker mid-analysis after ~ptr_map is a
    // use-after-destroy (observed: chat_room SIGABRT, worker freeing a
    // destroyed g_cache slot during grow()). atexit handlers and static
    // destructors share one finalization list run in reverse registration
    // order; this registration happens at first runtime init — after all
    // static initialization — so it fires before every static destructor.
    static bool atexit_registered =
        (std::atexit(stop_stack_analysis_worker), true);
    (void)atexit_registered;
    ws.stop = false;
    ws.thr = std::thread(analysis_worker_main);
    ws.running = true;
}

void stop_stack_analysis_worker() {
    auto& ws = worker_state();
    {
        std::lock_guard<std::mutex> lk(ws.mu);
        if (!ws.running) return;
        ws.stop = true;
        ws.cv.notify_one();
    }
    ws.thr.join();
    std::lock_guard<std::mutex> lk(ws.mu);
    ws.running = false;
    ws.stop = false;
}

} // namespace detail

namespace internal {

void analysis_quiesce() {
    auto& ws = worker_state();
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(ws.mu);
            if (!ws.running) return;
            ws.cv.notify_one();  // cut the worker's poll interval short
        }
        if (analysis_queue_empty() &&
            !g_analysis_in_flight.load(std::memory_order_seq_cst)) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

} // namespace internal

} // namespace csp

#else // !__aarch64__

namespace csp {

stack_analysis analyze_stack_depth(const void* fn, const void* data,
                                  stack_analysis_options opts) {
    // Non-ARM64: return conservative default.
    return {32u << 10, false};
}

stack_analysis analyze_stack_depth_cached(const void* fn, const void* data,
                                          stack_analysis_options opts) {
    return {32u << 10, false};
}

namespace detail {

// 🎯T52.4: no walker on this architecture — the stub is the same
// conservative sentinel as the sync API, and there is nothing for a
// worker to compute, so the lifecycle hooks are no-ops.
stack_analysis stack_analysis_lookup_or_request(const void*) noexcept {
    return {32u << 10, false};
}

void start_stack_analysis_worker() {}
void stop_stack_analysis_worker() {}

} // namespace detail

namespace internal {

void analysis_quiesce() {}

} // namespace internal

} // namespace csp

#endif
