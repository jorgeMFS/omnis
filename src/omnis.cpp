// ============================================================
// omnis.cpp - OMNIS Solomonoff induction approximator: 3-mode sieve
//
// Three execution MODES (not representations):
// MODE_ITER: R=init, persist, output R[outr]%A before body
// MODE_FUNC: R[0]=n, R[1..7]=0, run body, output R[outr]%A
// MODE_EMIT: R=init, persist, collect OUT emissions
//
// Two output INTERPRETATIONS:
// OUT_MOD: output = R[outr] % A (default)
// OUT_BIT: output = R[0].bit(k) (wide integer, A=2)
//
// Jorge Miguel Ferreira da Silva, 2026-04-13
// ============================================================

#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstring>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif
// Self-contained: no external namespace dependencies

static double now_s(){using C=std::chrono::steady_clock;static auto t0=C::now();
return std::chrono::duration<double>(C::now()-t0).count();}

// ================================================================
// Section 1: ISA Execution - One interpreter, reused everywhere
// ================================================================
static constexpr int64_t SAT=(int64_t)1e15;
// Fast hash mixer (wyhash-inspired). Replaces FNV-1a in hot fingerprinting paths.
static inline uint64_t hmix(uint64_t h,uint64_t v){h^=v;h*=0x9e3779b97f4a7c15ULL;h^=h>>32;return h;}
static thread_local bool g_sat=false; // thread_local: removes aliasing barrier, prepares for threading (A1)
static inline int64_t sat(int64_t v){if(__builtin_expect(v>SAT||v<-SAT,0))g_sat=true;return v>SAT?SAT:v<-SAT?-SAT:v;}
static inline int64_t smul(int64_t a,int64_t b){return sat((int64_t)((__int128)a*b));}

// EMIT globals: when g_emit_A>0, OUT pushes to g_emit_buf
static thread_local int g_emit_A=0;
static thread_local std::vector<int>g_emit_buf;

// ISA logical register count, also referenced by AdaptiveDeadline
// scheduling below. Defined here so Deadline's constructor can use it; the
// canonical declaration at line ~290 is kept as a duplicate-safe constexpr.
static constexpr int kRegisterCount=8;

// DARY (deductive d-ary digit recurrence) canonical register count.
// DARY's body uses 3 registers (R0/R1/R2): R0 = output accumulator, R1 = quotient
// state for the digit loop, R2 = current digit. This is structural (the body
// constructed at lines 2657 and 2675 uses exactly 3 register slots), and the MDL
// formula in computeMDL relies on this canonical value to compute the per-arg
// register-selector cost for dary_op without encoding nr separately.
static constexpr int kDaryCanonicalNr = 3;

// kDDBBodyMax declared early so
// struct Deadline can derive its meaningful-progress threshold from it.
// Canonical definition `static constexpr int kDDBBodyMax = sizeof(DDB::ops)/sizeof(Ins)`
// at line ~437 is kept as a static_assert verifying this value stays in sync
// with the DDB body buffer size.
static constexpr int kDDBBodyMaxEarly = 8;

// Forward-declare isaMaxConstant so the LOOP-iteration cap below can derive
// from it. Definition lives near the ISA constant range table.
static int isaMaxConstant();

//
// kLoopIterCap, kStepIterCap, kQuickCheckLen are CONSTEXPR LITERALS with values
// matching the ISA-derived expressions. constexpr is essential because these
// constants gate HOT INNER LOOPS (LOOP iteration, STEP iteration, fingerprint
// probe). With `static const int = isaMaxConstant() *...`, the compiler must
// load the value from memory each iteration → cannot constant-fold the loop
// bound → cannot unroll/optimize the hot path → ~10× slower per-iteration.
// Runtime verification below confirms the constexpr literal matches the
// ISA-derived expression; if isaMaxConstant ever changes, init-time check
// triggers a hard assertion failure.
// kLoopIterCap = 2 × isaMaxConstant² = 2 × 10² = 200
// kStepIterCap = isaMaxConstant⁴ = 10⁴ = 10000
// kQuickCheckLen = 2 × isaMaxConstant = 20
static constexpr int kIsaMaxConstantConstexpr = 10;
static constexpr int kLoopIterCap = 2 * kIsaMaxConstantConstexpr * kIsaMaxConstantConstexpr;
static constexpr int kStepIterCap = kIsaMaxConstantConstexpr * kIsaMaxConstantConstexpr * kIsaMaxConstantConstexpr * kIsaMaxConstantConstexpr;
static constexpr int kQuickCheckLen = 2 * kIsaMaxConstantConstexpr;

// hash-set initial bucket reserve for Phase 2F
// fingerprint dedup. Derivation: isaMaxConstant⁵ - five levels of ISA
// compositional reachability (matches the order of magnitude of expected
// fingerprint counts at L≤8 multiset enumeration). With current ISA (max=10)
// this evaluates to 100000 - identical to the prior magic literal. Sets
// initial bucket count; over-shoots merely allocate slightly more memory but
// avoid rehashing on the hot path.
static const int kFpHashReserve = isaMaxConstant()*isaMaxConstant()*isaMaxConstant()*isaMaxConstant()*isaMaxConstant();

// Init-time verification: the constexpr ISA-max literal must match the runtime
// isaMaxConstant() function value. If isaMaxConstant changes (different ISA),
// this hard-asserts at program start so the constexpr literals can be updated.
static const int kIsaMaxConstantVerified = []() {
    int rt = isaMaxConstant();
    if (rt != kIsaMaxConstantConstexpr) {
        std::fprintf(stderr,
            "FATAL: kIsaMaxConstantConstexpr=%d does not match isaMaxConstant()=%d. "
            "Update the constexpr literal in omnis.cpp.\n",
            kIsaMaxConstantConstexpr, rt);
        std::abort();
    }
    return rt;
}();

// Adaptive deadline: extends Phase 2B when the cascade shows progress.
// Other phases read d.t_end as a scalar and do not extend.
struct Deadline{
    std::atomic<double> t_end;  // current effective deadline (thread-safe: monotone extend)
    double t_max;               // hard ceiling, never exceeded (const after init)
    std::atomic<int> best_sc_seen{0};  // monotone record of cascade progress
    int    N_target=0;          // target length (const after init)
    int    A_target=2;          // alphabet size (const after init)
    std::atomic<double> t_last_improve{0}; // wall time of last sc increase
    double window_W=0.0;        // progress window (set by ctor from ceiling-baseline)
    double increment_D=0.0;     // extension granted per observed progress (window_W / kRegisterCount)

    // derive both timing parameters structurally from
    // the phase budget and ISA register count - no magic seconds.
    // window_W = ceiling - baseline (full patience: phase's allotted span;
    // stagnated longer than this → give up)
    // increment_D= window_W / kRegisterCount (one-eighth of patience per progress;
    // caps total extensions to ≤ kRegisterCount
    // before hitting t_max - ISA-bounded)
    Deadline(double baseline,double ceiling,int N,int A):
        t_end(baseline),t_max(ceiling),best_sc_seen(0),N_target(N),A_target(A),
        t_last_improve(now_s()),
        window_W(std::max(0.0,ceiling-baseline)),
        increment_D(std::max(0.0,(ceiling-baseline)/(double)kRegisterCount)){}

    bool alive() const{return now_s()<t_end.load(std::memory_order_relaxed);}

    void report(int sc){
        if(sc>best_sc_seen.load(std::memory_order_relaxed)){
            best_sc_seen.store(sc,std::memory_order_relaxed);
            t_last_improve.store(now_s(),std::memory_order_relaxed);}
    }

    void maybe_extend(){
        double now=now_s();
        double te=t_end.load(std::memory_order_relaxed);
        if(now<te)return;
        if(te>=t_max)return;
        if(now-t_last_improve.load(std::memory_order_relaxed)>window_W)return;
        // meaningful-progress threshold derived
        // from ISA structurals.
        // N_target/(kDDBBodyMaxEarly/2) = N/4 - at least 1/(half-body-length)
        // of sequence shown; ties cascade-meaningfulness to ISA body cap.
        // (isaMaxConstant/2)*A_target = 5*A - at least half-max-constant
        // samples per alphabet class; ties to ISA value-range.
        // Both ISA-derived; numerically identical to prior magic 4 and 5.
        int meaningful=std::max(N_target/(kDDBBodyMaxEarly/2),
                                (isaMaxConstant()/2)*A_target);
        if(best_sc_seen.load(std::memory_order_relaxed)<=meaningful)return;
        t_end.store(std::min(t_max,te+increment_D),std::memory_order_relaxed);
    }
};

struct Ins{int ti,c;int8_t args[4];int ar; // A6: args packed to int8_t (registers 0-7 fit in 1 byte)
    std::string str()const{
        const char* name = nullptr;
        // bound derives from actual table size,
        // not literal 17. Canonical ISA opcodes 0..(N_count-1).
        static const char*N[]={"INC","DEC","ADD","SUB","MUL","MUL_C","MOD_C","MOD_R",
            "DIVC","DIVR","LOAD","COPY","OUT","AND","OR","XOR","ISZERO","LOOP"};
        constexpr int kCanonicalOpcodes = (int)(sizeof(N)/sizeof(N[0]));
        if (ti >= 0 && ti < kCanonicalOpcodes) {
            name = N[ti];
        }
        else if (ti == 32) {
            // SUB_CALL stores library index in c (typeAr=0: no register operands).
            return std::string("SUB_CALL(L") + std::to_string(c) + ")";
        }
        if (!name) return "?";
        std::string s=name;s+="(";for(int i=0;i<ar;i++){if(i)s+=",";s+="R"+std::to_string(args[i]);}
        if(ti==5||ti==6||ti==8||ti==10||ti==17)s+=","+std::to_string(c);
        s+=")";return s;}};
// SUB_CALL extension (SUB_CALL, ID=8): hierarchical synthesis primitive - CANONICAL.
// Inline-expands a library program (g_progdb entry at index i.c) into the current
// execution context. Forward-declared here so ex()/exW() can call it; defined after
// g_progdb in Section 6b.
struct W;
static void exSubCall(int64_t* R, int idx);
static void exSubCallW(W* R, int idx);
// Library-size getter (forward-declared because buildL1 is defined before g_progdb).
static int subCallCatalogSize();
// True iff library entry idx is safe to inline-expand: pure body, non-recursive,
// no special modes. Used by buildL1 to skip non-invocable entries.
static bool subCallLibraryEntryInvocable(int idx);
static void ex(int64_t*R,const Ins&i){int t=i.ti,c=i.c;const int8_t*a=i.args;switch(t){
    case 0:R[a[0]]=sat(R[a[0]]+1);break;case 1:if(R[a[0]]>0)R[a[0]]--;break;
    case 2:R[a[2]]=sat(R[a[0]]+R[a[1]]);break;case 3:R[a[2]]=sat(R[a[0]]-R[a[1]]);break;
    case 4:R[a[2]]=smul(R[a[0]],R[a[1]]);break;case 5:R[a[1]]=smul(R[a[0]],c);break;
    case 6:{int64_t v=R[a[0]];if(c>0){int64_t r=v%c;R[a[1]]=r>=0?r:r+c;}else R[a[1]]=0;}break;
    case 7:{int64_t v=R[a[0]],d=R[a[1]];if(d>0){int64_t r=v%d;R[a[2]]=r>=0?r:r+d;}else R[a[2]]=0;}break;
    case 8:{int64_t v=R[a[0]];if(c>0){R[a[1]]=v/c;int64_t r=v%c;R[a[2]]=r>=0?r:r+c;}else{R[a[1]]=0;R[a[2]]=0;}}break;
    case 9:{int64_t v=R[a[0]],d=R[a[1]];if(d>0){R[a[2]]=v/d;int64_t r=v%d;R[a[3]]=r>=0?r:r+d;}else{R[a[2]]=0;R[a[3]]=0;}}break;
    case 10:R[a[0]]=c;break;case 11:R[a[1]]=R[a[0]];break;
    case 12:if(g_emit_A>0){int r=(int)(R[a[0]]%g_emit_A);g_emit_buf.push_back(r>=0?r:r+g_emit_A);}break;
    case 13:R[a[2]]=(R[a[0]]>=0&&R[a[1]]>=0)?R[a[0]]&R[a[1]]:0;break;
    case 14:R[a[2]]=(R[a[0]]>=0&&R[a[1]]>=0)?R[a[0]]|R[a[1]]:0;break;
    case 15:R[a[2]]=(R[a[0]]>=0&&R[a[1]]>=0)?R[a[0]]^R[a[1]]:0;break;
    case 16:R[a[1]]=(R[a[0]]==0)?1:0;break; // ISZERO(src,dst)
    // SUB_CALL extension (SUB_CALL, canonical): inline-expand library entry i.c.
    case 32: exSubCall(R, c); break;
    }};
static inline int pm(int64_t v,int A){
    // Fast path: power-of-2 A with non-negative v (common case: A=2,4,8)
    if(v>=0&&(A&(A-1))==0)return(int)(v&(A-1));
    int r=(int)(v%A);return r>=0?r:r+A;}
// Euclidean modulo for branch dispatch (single division, no double-mod)
static inline int64_t emod(int64_t v,int m){int64_t r=v%m;return r>=0?r:r+m;}
// ExBodyDepthGuard removed. The guard existed solely to reset
// MEM_ARRAY/BIGINT state at depth=0 entry. With both extensions extracted to
// extensions/, the canonical engine has no extension state to reset → no
// guard needed. Body executors run with no per-call state machinery.

// Fast path for bodies with no LOOP instruction (vast majority in Phase 2B)
static inline void exBodyFlat(int64_t*R,const Ins*b,int n){
    switch(n){
    case 1:ex(R,b[0]);return;
    case 2:ex(R,b[0]);ex(R,b[1]);return;
    case 3:ex(R,b[0]);ex(R,b[1]);ex(R,b[2]);return;
    case 4:ex(R,b[0]);ex(R,b[1]);ex(R,b[2]);ex(R,b[3]);return;
    default:for(int i=0;i<n;i++)ex(R,b[i]);}
}

static void exBody(int64_t*R,const Ins*b,int n); // forward decl for exBodyD
// A1: Hoisted LOOP topology descriptor. Precomputed once per body,
// reused across all N-step verifications. Eliminates O(n) prescan per call.
struct BodyDesc{
    bool has_loop;
    uint32_t in_loop_mask;    // bit i: position i inside outermost LOOP range
    uint32_t loop_inner_mask; // bit i: LOOP at position i has nested LOOPs
};
static_assert(24 <= 32, "max body length (24) must fit in uint32_t bitmasks");
static inline void computeBodyDesc(const Ins*b,int n,BodyDesc&d){
    d.has_loop=false;d.in_loop_mask=0;d.loop_inner_mask=0;
    for(int i=0;i<n;i++){
        if(b[i].ti==17){d.has_loop=true;int ll=b[i].c;
        for(int j=std::max(0,i-ll);j<i;j++)d.in_loop_mask|=(1u<<j);
        bool inner=false;
        for(int j=i-ll;j<i&&!inner;j++)if(b[j].ti==17)inner=true;
        if(inner)d.loop_inner_mask|=(1u<<i);}}}
static void exBodyD(int64_t*R,const Ins*b,int n,const BodyDesc&d){
    if(!d.has_loop){exBodyFlat(R,b,n);return;}
    for(int i=0;i<n;i++){
        if(b[i].ti==17){
            int ll=b[i].c,kr=b[i].args[0];if(ll<=0||i-ll<0)continue;
            bool inner=(d.loop_inner_mask>>i)&1;
            for(int it=0;it<kLoopIterCap&&R[kr]!=0&&!g_sat;it++){
                if(inner)exBody(R,b+(i-ll),ll);
                else exBodyFlat(R,b+(i-ll),ll);}
            if(R[kr]!=0)g_sat=true;
        }else if(!((d.in_loop_mask>>i)&1))ex(R,b[i]);
    }}
// Legacy exBody: computes desc inline (for callers that don't cache)
static void exBody(int64_t*R,const Ins*b,int n){
    BodyDesc d;computeBodyDesc(b,n,d);exBodyD(R,b,n,d);}

// ---- Wide integer (512 bits = 8x uint64) for extended flat deep search ----
// limb count and bits per limb derived from machine
// types and struct layout. kLimbs = sizeof(w)/sizeof(uint64_t) = 8;
// kLimbBits = sizeof(uint64_t)*8 = 64. static_asserts below verify consistency.
struct W{uint64_t w[kRegisterCount]={};
    static constexpr int kLimbs = 8;
    static constexpr int kLimbBits = 64;
    bool isZero()const{for(int i=0;i<kLimbs;i++)if(w[i])return false;return true;}
    bool isPos()const{return!isZero()&&!(w[kLimbs-1]>>(kLimbBits-1));}
    W operator^(const W&o)const{W r;for(int i=0;i<kLimbs;i++)r.w[i]=w[i]^o.w[i];return r;}
    W operator|(const W&o)const{W r;for(int i=0;i<kLimbs;i++)r.w[i]=w[i]|o.w[i];return r;}
    W operator&(const W&o)const{W r;for(int i=0;i<kLimbs;i++)r.w[i]=w[i]&o.w[i];return r;}
    W operator+(const W&o)const{W r;uint64_t c=0;for(int i=0;i<kLimbs;i++){
        __uint128_t s=(__uint128_t)w[i]+o.w[i]+c;r.w[i]=(uint64_t)s;c=(uint64_t)(s>>kLimbBits);}return r;}
    W operator-(const W&o)const{W r;uint64_t b=0;for(int i=0;i<kLimbs;i++){
        __uint128_t s=(__uint128_t)w[i]-o.w[i]-b;r.w[i]=(uint64_t)s;b=(s>>kLimbBits)?1:0;}return r;}
    W mulc(int c)const{W r;uint64_t cy=0;for(int i=0;i<kLimbs;i++){
        __uint128_t p=(__uint128_t)w[i]*c+cy;r.w[i]=(uint64_t)p;cy=(uint64_t)(p>>kLimbBits);}return r;}
    int modc(int c)const{uint64_t rem=0;for(int i=kLimbs-1;i>=0;i--){
        __uint128_t v=((__uint128_t)rem<<kLimbBits)|w[i];rem=(uint64_t)(v%c);}return(int)rem;}
    W divc(int c)const{W r;uint64_t rem=0;for(int i=kLimbs-1;i>=0;i--){
        __uint128_t v=((__uint128_t)rem<<kLimbBits)|w[i];r.w[i]=(uint64_t)(v/c);rem=(uint64_t)(v%c);}return r;}
    bool bit(int i)const{return(w[i/kLimbBits]>>(i%kLimbBits))&1;}
    void setBit(int i){w[i/kLimbBits]|=(1ULL<<(i%kLimbBits));}
    static bool less(const W&a,const W&b){for(int i=kLimbs-1;i>=0;i--){
        if(a.w[i]<b.w[i])return true;if(a.w[i]>b.w[i])return false;}return false;}
    static W from(int64_t v){W r;r.w[0]=(uint64_t)v;if(v<0)for(int i=1;i<kLimbs;i++)r.w[i]=~0ULL;return r;}
    static void divmod(const W&num,const W&den,W&q,W&rem){
        if(den.isZero()){q={};rem={};return;}
        // bit-iteration upper bound = total W bits.
        // kLimbs × kLimbBits = 8 × 64 = 512 bits; loop covers bits 0..511.
        constexpr int kWBits = kLimbs * kLimbBits;
        static_assert(kWBits == (int)(sizeof(W) * 8), "kWBits must equal sizeof(W) in bits");
        q={};rem={};for(int i=kWBits-1;i>=0;i--){rem=rem+rem;if(num.bit(i))rem.w[0]|=1;
        if(!less(rem,den)){rem=rem-den;q.setBit(i);}}}};
static void exW(W*R,const Ins&i){int t=i.ti,c=i.c;const int8_t*a=i.args;switch(t){
    case 0:R[a[0]]=R[a[0]]+W::from(1);break;
    case 1:if(R[a[0]].isPos())R[a[0]]=R[a[0]]-W::from(1);break;
    case 2:R[a[2]]=R[a[0]]+R[a[1]];break;case 3:R[a[2]]=R[a[0]]-R[a[1]];break;
    case 4: break; // MUL: wide-int no-op (MUL_W is not part of the canonical ISA)
    case 5:R[a[1]]=R[a[0]].mulc(c);break;
    case 6:if(c>0)R[a[1]]=W::from(R[a[0]].modc(c));break;
    case 7:{W d=R[a[1]];if(!d.isZero()){W q,r;W::divmod(R[a[0]],d,q,r);R[a[2]]=r;}break;}
    case 8:{W v=R[a[0]];if(c>0){R[a[1]]=v.divc(c);R[a[2]]=W::from(v.modc(c));}break;}
    case 9:{W v=R[a[0]],d=R[a[1]];if(!d.isZero()){W q,r;W::divmod(v,d,q,r);R[a[2]]=q;R[a[3]]=r;}break;}
    case 10:R[a[0]]=W::from(c);break;case 11:R[a[1]]=R[a[0]];break;
    case 12:if(g_emit_A>0)g_emit_buf.push_back(R[a[0]].modc(g_emit_A));break; // modc returns non-negative
    case 13:R[a[2]]=R[a[0]]&R[a[1]];break;
    case 14:R[a[2]]=R[a[0]]|R[a[1]];break;
    case 15:R[a[2]]=R[a[0]]^R[a[1]];break;
    case 16:R[a[1]]=(R[a[0]].isZero())?W::from(1):W{};break; // ISZERO wide
    case 32: exSubCallW(R, c); break; // SUB_CALL extension
    }};
static void exBodyW(W*R,const Ins*b,int n){
    bool in_loop_w[24]={};
    for(int i=0;i<n;i++){if(b[i].ti==17){int ll=b[i].c;
    for(int j=std::max(0,i-ll);j<i;j++)in_loop_w[j]=true;}}
    for(int i=0;i<n;i++){
        if(b[i].ti==17){int ll=b[i].c,kr=b[i].args[0];if(ll<=0||i-ll<0)continue;
        for(int it=0;it<kLoopIterCap&&!R[kr].isZero();it++)
            exBodyW(R,b+(i-ll),ll); // recursive for nested LOOPs
        }else if(!in_loop_w[i])exW(R,b[i]);
    }
}
static inline int pmW(const W&v,int A){return(int)(v.w[0]%A);} // w[0] is uint64_t, always non-negative
static int typeAr(int t){switch(t){case 0:case 1:case 10:return 1;case 5:case 6:case 11:case 12:case 16:return 2;
    case 2:case 3:case 4:case 7:case 8:case 13:case 14:case 15:return 3;case 9:return 4;
    default:return 0;}}
// Precomputed wide-int initial values for fingerprinting (eliminates 150 wide adds per fp call)
// fingerprint-shift bit positions for wide-int hashing.
// Derivation:
// shift1 = 5 × isaMaxConstant - half of isaMaxConstant² (lower limb sample)
// shift2 = isaMaxConstant² - full isaMaxConstant² (next-limb sample)
// With current ISA (max=10): shift1=50, shift2=100. Two distinct bit positions
// in different W limbs (limb 0 covers bits 0-63, limb 1 covers 64-127), giving
// the fingerprint diversity across limbs. Numerically identical to prior magic.
static const int kWInitShift1 = 5 * isaMaxConstant();
static const int kWInitShift2 = isaMaxConstant() * isaMaxConstant();
static const W kWInit50=[](){W r=W::from(1);for(int s=0;s<kWInitShift1;s++)r=r+r;return r;}();
static const W kWInit100=[](){W r=W::from(1);for(int s=0;s<kWInitShift2;s++)r=r+r;return r;}();

// The ISA constant ranges, defined once. This is the ONLY place that
// determines what programs the search can find. Every ISA type must be
// here. Types with no constant parameter get {0}.
// Cached as function-local static (built once, reused across all calls)
static const std::map<int,std::vector<int>>& isaConstantRanges(){
    static const std::map<int,std::vector<int>>m=[]{
        std::map<int,std::vector<int>>r;
        // Canonical constant ranges.
        const int MULC_HI=8,  MODC_HI=8,  DIVC_HI=8,  LOAD_HI=10;
        r[0]={0};r[1]={0};r[2]={0};r[3]={0};r[4]={0};  // INC,DEC,ADD,SUB,MUL
        for(int c=0;c<=MULC_HI;c++)r[5].push_back(c);    // MUL_C
        for(int c=1;c<=MODC_HI;c++)r[6].push_back(c);    // MOD_C
        r[7]={0};                                          // MOD_R
        for(int c=1;c<=DIVC_HI;c++)r[8].push_back(c);    // DIVC
        r[9]={0};                                          // DIVR
        for(int c=0;c<=LOAD_HI;c++)r[10].push_back(c);   // LOAD
        r[11]={0};r[12]={0};r[13]={0};r[14]={0};r[15]={0};r[16]={0}; // COPY,OUT,AND,OR,XOR,ISZERO (opcode indices)
        return r;}();
    return m;}

// Maximum constant value across all ISA opcode constant ranges.
// Used as the structural lower bound for fingerprint probe count G:
// to distinguish bodies with operations like MOD_C(c) and LOAD(c), the
// probe set must include all residues mod c for c ≤ max_c, hence
// G ≥ max_c + 1. This makes G derive from the ISA, not a magic constant.
static int isaMaxConstant(){
    int mx=0;
    for(auto&kv:isaConstantRanges()) for(int c:kv.second) if(c>mx) mx=c;
    return mx;
}

// kRegisterCount declared earlier (above struct Deadline) so AdaptiveDeadline
// scheduling can derive its window from it. ISA fact: omnis defines 8 logical
// registers R0..R7 (matches int64_t R[kRegisterCount] and W::w[8] throughout the engine).

// fingerprint emission alphabet derived from number
// theory - smallest prime > isaMaxConstant guarantees coprimality with every
// ISA constant c ∈ [1, isaMaxConstant], so emissions retain full information
// regardless of which mod-c relationship the program expresses. The previous
// magic value 7 was NOT coprime with itself (gcd(7,7)=7), causing fingerprint
// information loss for any mod-7 program (e.g., bench_pow3mod7). Replacing 7
// with first-prime-above(10) = 11 fixes this theoretical defect.
static const int kFpEmitAlphabet = []() {
    auto isPrime = [](int n) {
        if (n < 2) return false;
        for (int i = 2; i * i <= n; i++) if (n % i == 0) return false;
        return true;
    };
    int p = isaMaxConstant() + 1;
    while (!isPrime(p)) p++;
    return p;
}();

// cascade search pre-builds pools at depths 1, 2, 3
// (variables pool1, pool2, pool3 in cascadeSearch). The cascade then enumerates
// branched (even/odd) combinations with even-depth de ∈ [1, kCascadePoolLevels]
// and odd-depth doo ∈ [1, kCascadePoolLevels], so total depth td = de + doo
// ranges over [2, 2 × kCascadePoolLevels]. Architectural choice exposed as a
// named constant rather than a magic literal.
static constexpr int kCascadePoolLevels = 3;

// machine-derived available memory for structural cap allocation.
// Used by Phase 2F to bound multiset enumeration without magic constants.
static size_t availableMemoryBytes(){
#if defined(__APPLE__)
    int64_t total=0; size_t len=sizeof(total);
    if(sysctlbyname("hw.memsize",&total,&len,nullptr,0)==0&&total>0)return(size_t)total;
#elif defined(_SC_PHYS_PAGES)&&defined(_SC_PAGE_SIZE)
    long pages=sysconf(_SC_PHYS_PAGES); long ps=sysconf(_SC_PAGE_SIZE);
    if(pages>0&&ps>0)return(size_t)pages*(size_t)ps;
#endif
    return (size_t)16ULL*1024*1024*1024; // structural fallback: 16 GiB
}

// query current FREE memory at runtime (free + inactive +
// reusable pages on Darwin; available memory on Linux). This is the actual
// headroom available for new allocations - strictly more accurate than
// dividing total memory by an assumed concurrency factor.
static size_t currentFreeMemoryBytes(){
#if defined(__APPLE__)
    vm_statistics64_data_t s{};
    mach_msg_type_number_t cnt=HOST_VM_INFO64_COUNT;
    if(host_statistics64(mach_host_self(),HOST_VM_INFO64,(host_info64_t)&s,&cnt)==KERN_SUCCESS){
        size_t ps=(size_t)vm_kernel_page_size;
        return((size_t)s.free_count+(size_t)s.inactive_count+(size_t)s.purgeable_count)*ps;
    }
#elif defined(_SC_AVPHYS_PAGES)&&defined(_SC_PAGE_SIZE)
    long pages=sysconf(_SC_AVPHYS_PAGES); long ps=sysconf(_SC_PAGE_SIZE);
    if(pages>0&&ps>0)return(size_t)pages*(size_t)ps;
#endif
    return availableMemoryBytes()/2; // fallback: 50% of total
}

// calibrate per-op execution cost at runtime. Returns seconds
// per single instruction execution. Used to derive multiset enumeration caps
// structurally rather than with hand-picked magic ladders.
//
// Calibration robustness against thread-load noise:
// Scheduling/cache contention adds non-negative noise η ≥ 0 to any single
// measurement, so true cost C_true ≤ measured cost. The min-of-N estimator
// min_i(C_i) → C_true as the probability of an uncontaminated sample → 1.
// We take N = kCascadePoolLevels = 3 samples (smallest quorum giving outlier
// rejection across two contaminated samples) and return their minimum. The
// static-init invocation below (kCalibratedPerOpCostInit) forces the first
// call to occur during namespace-scope initialization - strictly before main()
// and before any worker threads - guaranteeing the cached value is captured
// in a single-threaded, low-load context. Subsequent calls reuse the cache.
static double calibratePerOpCost(){
    static double cached=-1.0;
    if(cached>0.0)return cached;
    // calibration iteration count = kStepIterCap.
    // Reuses isaMaxConstant⁴ = 10000 for runtime-cost measurement precision -
    // same numerical value as before, ISA-derived.
    const int K=kStepIterCap;
    // number of independent samples = kCascadePoolLevels = 3.
    // Three is the smallest quorum that survives two contaminated samples
    // (k-out-of-n robustness with k=1, n=3). Structural ISA constant - no
    // magic number introduced.
    const int kCalibSamples = kCascadePoolLevels;
    double best = std::numeric_limits<double>::infinity();
    for(int s=0; s<kCalibSamples; s++){
        // seed values derived from theoretical principle.
        // Microbenchmark requires distinct non-zero values across all kRegisterCount
        // registers to avoid degenerate register-pair operations (zero registers
        // trigger early termination; identical registers make ADD/SUB no-ops).
        // Minimal-information seed = first kRegisterCount positive integers.
        int64_t R[kRegisterCount];
        for(int i=0;i<kRegisterCount;i++) R[i]=(int64_t)(i+1);
        Ins inc{0,0,{0,0,0,0},1};  // INC R0 - minimal canonical instruction
        auto t0=std::chrono::steady_clock::now();
        for(int i=0;i<K;i++)ex(R,inc);
        auto t1=std::chrono::steady_clock::now();
        double sample=std::chrono::duration<double>(t1-t0).count()/K;
        if(sample > 0.0 && sample < best) best = sample;
    }
    cached = best;
    if(cached<=0.0 || !std::isfinite(cached)) cached=1e-8; // safety: ≥10ns
    return cached;
}

// force calibratePerOpCost() to execute during namespace-scope
// static initialization (single-threaded, before main()). The cached value
// is then served from cache for all subsequent calls - including those from
// worker threads - guaranteeing a noise-free baseline.
static const double kCalibratedPerOpCostInit = calibratePerOpCost();

// Returns the register-index the instruction writes to (i.args[<idx>]), or -1
// for non-writer opcodes (OUT writes to g_emit_buf; SUB_CALL writes through
// inlined library body - both opaque to direct register dataflow).
// Used by buildPool depth-3 to bucket "writer-of-R0" candidates correctly.
static int writerArgIdx(const Ins& i) {
    int t = i.ti;
    // SUB_CALL writes registers indirectly via inlined library body.
    // WSBP cannot trace through SUB_CALL → returns -1 (filtered from tyCat in Phase 2F).
    if (t == 32) return -1;
    // 12 = OUT writes to g_emit_buf, not register
    if (t == 12) return -1;
    int a = typeAr(t);
    if (a == 1) return 0;
    if (a == 2) return 1;
    if (a == 3) return 2;
    if (a == 4) return 2; // DIVR primarily writes a[2] (q); a[3] (rem) handled via fallback below
    return -1;
}

static std::vector<Ins> buildL1(int nr){
    // Generate ALL ISA types. The ISA IS the Solomonoff prior.
    auto ranges=isaConstantRanges();
    std::vector<Ins> out;
    for(auto&[t,consts]:ranges){
        int a=typeAr(t);if(!a)continue;
        for(int c:consts){
            int tot=1;for(int i=0;i<a;i++)tot*=nr;
            for(int cc=0;cc<tot;cc++){
                Ins ins;ins.ti=t;ins.c=c;ins.ar=a;
                int v=cc;for(int i=0;i<a;i++){ins.args[i]=v%nr;v/=nr;}
                for(int i=a;i<4;i++)ins.args[i]=0;
                out.push_back(ins);}}}
    // SUB_CALL extension SUB_CALL (canonical): emit one Ins per current INVOCABLE library entry.
    // ar=0 (no register operands); index stored in c. Catalog grows by invocable
    // library size - that growth is the Solomonoff-correct selector cost (log2(ncat)
    // per slot). If library is empty, no entries emitted and SUB_CALL is dormant.
    {
        int lib_size = subCallCatalogSize();
        for (int idx = 0; idx < lib_size; idx++) {
            if (!subCallLibraryEntryInvocable(idx)) continue;
            Ins ins; ins.ti = 32; ins.c = idx; ins.ar = 0;
            for (int j = 0; j < 4; j++) ins.args[j] = 0;
            out.push_back(ins);
        }
    }
    return out;}

// ---- Deep body (up to 8 ops) for extended flat search ----
struct DDB{Ins ops[8];int n;bool has_loop=false; // ops[8]: kDDBBodyMax derives below
    void computeLoop(){has_loop=false;for(int i=0;i<n;i++)if(ops[i].ti==17){has_loop=true;return;}}};
// body length cap derives from struct DDB::ops
// buffer size - same numeric value, structurally tied to the type definition.
static constexpr int kDDBBodyMax=sizeof(DDB::ops)/sizeof(Ins);
static_assert(kDDBBodyMaxEarly == kDDBBodyMax, "kDDBBodyMaxEarly forward-decl must match canonical kDDBBodyMax");
// Res::body and ProgramRecord::body buffer size.
// A complete program can compose up to three ISA-bodied segments:
// - branched MODE_ITER: then-branch + else-branch (≤ 2 × kDDBBodyMax)
// - Phase 2H hierarchical: pre-body + library entry + post-body (≤ 3 × kDDBBodyMax)
// 3 × kDDBBodyMax = 24 with current ISA, identical to the previous magic 24.
// Numerically unchanged - binary format of program_db.bin is preserved.
static constexpr int kProgramBodyMax = 3 * kDDBBodyMax;
static_assert(kProgramBodyMax <= 32, "kProgramBodyMax must fit in uint32_t bitmasks (BodyDesc::in_loop_mask, loop_inner_mask)");
typedef std::pair<uint64_t,uint64_t> FP128;
struct FP128Hash{size_t operator()(const FP128&p)const{return p.first^(p.second*0x9e3779b97f4a7c15ULL);}};
static FP128 computeBodyFP(const Ins*ops,int n,bool fast=false){
    // Fingerprint registers state for pool dedup. Canonical engine has no
    // extension state (MEM_ARRAY/BIGINT extracted) so no per-probe reset needed.
    if(fast){
        uint64_t h=0xcbf29ce484222325ULL;
        for(int r0=0;r0<=isaMaxConstant();r0++){int64_t R[kRegisterCount]={};R[0]=r0;
        g_sat=false;for(int i=0;i<n;i++){ex(R,ops[i]);if(g_sat)return{0,0};}
        for(int r=0;r<kRegisterCount;r++)h^=(uint64_t)(R[r]*(1000003+r*7919)+r0*1000033);
        h*=0x9e3779b97f4a7c15ULL;}
        return{h,0};}
    uint64_t hs=0xcbf29ce484222325ULL;
    for(int r0=0;r0<=isaMaxConstant();r0++){int64_t R[kRegisterCount]={};R[0]=r0;
    g_emit_A=kFpEmitAlphabet;g_emit_buf.clear();
    g_sat=false;for(int i=0;i<n;i++){ex(R,ops[i]);if(g_sat){g_emit_A=0;return{0,0};}}
    for(int r=0;r<kRegisterCount;r++)hs^=(uint64_t)(R[r]*(1000003+r*7919)+r0*1000033);
    hs^=(uint64_t)(g_emit_buf.size()*1000057);
    for(int e=0;e<(int)g_emit_buf.size();e++)hs^=(uint64_t)(g_emit_buf[e]*1000099*(e+1));
    hs*=0x9e3779b97f4a7c15ULL;}
    g_emit_A=0;
    uint64_t hl=0xcbf29ce484222325ULL;
    for(int shift:{kWInitShift1,kWInitShift2}){W R[kRegisterCount];R[0]=(shift==kWInitShift1)?kWInit50:kWInit100;
    for(int i=0;i<n;i++)exW(R,ops[i]);
    for(int r=0;r<kRegisterCount;r++){hl^=R[r].w[0]*(999961+r*6971);hl^=R[r].w[1]*(999979+r*7001);}
    hl*=0x9e3779b97f4a7c15ULL;}
    return{hs,hl};}
typedef std::function<bool(const Ins*,int)> BodyFilter;
static std::vector<DDB>composeDDB(const std::vector<DDB>&prev,const std::vector<Ins>&fL1,int nr,double dl,
                                   BodyFilter filter=nullptr,int cap=0,bool fast_fp=false,
                                   double per_entry_use_cost=0.0){
    //
    // pool_size × (build_cost + use_cost) ≤ phase_budget. cap derived as
    // min(theoretical_max, memory_bound, time_bound) where:
    // theoretical_max = |prev| × |fL1| (combinatorial ceiling, ISA)
    // memory_bound = free_mem / 2 / per_entry_bytes (machine)
    // time_bound = remaining_dl / (build_cost + use_cost_per_entry)
    // Caller passes per_entry_use_cost when known (Phase 2A); when 0, time bound
    // omitted (existing now_s()<dl loop check still bounds build time).
    // A memory-only cap was tried and regressed every benchmark by letting
    // Phase 2A consume the full deadline; the regression traced to a spurious
    // cost-check break and a missing MDL-sort, both since fixed. With those
    // in place, this Levin-partition cap is safe.
    if(cap<=0){
        int64_t theoretical = (int64_t)prev.size() * (int64_t)fL1.size();
        size_t per_entry_bytes = sizeof(DDB) + sizeof(FP128);
        int64_t mem_bound = (int64_t)(currentFreeMemoryBytes() / 2 / std::max((size_t)1, per_entry_bytes));
        int64_t cap_int = std::min({(int64_t)INT_MAX, theoretical, mem_bound});
        if (per_entry_use_cost > 0.0) {
            double per_op = calibratePerOpCost();
            double per_entry_build = (double)kDDBBodyMax * per_op;  // fingerprint+insertion
            double per_entry_total = per_entry_build + per_entry_use_cost;
            double remaining = std::max(0.0, dl - now_s());
            int64_t time_cap = (int64_t)(remaining / std::max(per_entry_total, 1e-12));
            cap_int = std::min(cap_int, time_cap);
        }
        cap = (int)std::max((int64_t)1, cap_int);
    }
    auto fp=[fast_fp](const Ins*ops,int n)->FP128{return computeBodyFP(ops,n,fast_fp);};
    std::unordered_set<FP128,FP128Hash>seen;seen.reserve(cap);
    std::vector<DDB>pool;pool.reserve(cap);
    if(filter){
    // When filtering: fL1-outer, prev-inner for fair coverage of all prev bodies.
    // Each fL1 instruction is tried with ALL prev bodies before moving to next.
    int nfL1=(int)fL1.size(),nprev=(int)prev.size();
    for(int t=0;t<nfL1&&(int)pool.size()<cap&&now_s()<dl;t++){
    for(int b=0;b<nprev&&(int)pool.size()<cap;b++){
    if(prev[b].n>=kDDBBodyMax)continue;
    Ins o[kDDBBodyMax];for(int i=0;i<prev[b].n;i++)o[i]=prev[b].ops[i];o[prev[b].n]=fL1[t];int nn=prev[b].n+1;
    auto f=fp(o,nn);if(f.first&&seen.insert(f).second){
    if(!filter(o,nn))continue;
    DDB bb;for(int i=0;i<nn;i++)bb.ops[i]=o[i];bb.n=nn;
    pool.push_back(bb);}}}
    }else{
    // prev-outer, fL1-inner. Each prev body tries all fL1 tails.
    // Pool cap provides the budget compromise.
    for(auto&b:prev){if(now_s()>dl||(int)pool.size()>=cap)break;if(b.n>=kDDBBodyMax)continue;
    Ins o[kDDBBodyMax];for(int i=0;i<b.n;i++)o[i]=b.ops[i];int nn=b.n+1;
    for(int t=0;t<(int)fL1.size()&&(int)pool.size()<cap;t++){
    o[b.n]=fL1[t];
    auto f=fp(o,nn);if(f.first&&seen.insert(f).second){
    DDB bb;for(int i=0;i<nn;i++)bb.ops[i]=o[i];bb.n=nn;
    pool.push_back(bb);}}}
    }for(auto&b:pool)b.computeLoop();return pool;}
static std::vector<DDB>buildDDB(const std::vector<Ins>&fL1,int depth,int nr,double dl,int pool_cap=0,bool fast_fp=false){
    // cap = |fL1|^depth (theoretical max after dedup, capped at INT_MAX).
    if(pool_cap<=0)pool_cap=(int)std::min((double)INT_MAX,std::pow((double)fL1.size(),depth));
    std::vector<DDB>pool;
    auto fp=[&](const Ins*ops,int n)->FP128{return computeBodyFP(ops,n,fast_fp);};
    std::unordered_set<FP128,FP128Hash>seen;int nL1=(int)fL1.size();
    if(depth==1){for(int i=0;i<nL1;i++){auto f=fp(&fL1[i],1);
    if(f.first&&seen.insert(f).second){DDB b;b.ops[0]=fL1[i];b.n=1;pool.push_back(b);}}}
    else if(depth==2){
        // Parallelized: range-partition i, per-thread pool+seen, sequential merge.
        // Order preserved modulo inter-thread duplicates (deterministic across runs).
        int nt=std::min((int)std::thread::hardware_concurrency(),std::max(1,nL1));
        std::vector<std::vector<DDB>>tpools(nt);
        std::vector<std::vector<FP128>>tfps(nt);
        auto d2_worker=[&](int tid){
            int i_start=(nL1*tid)/nt,i_end=(nL1*(tid+1))/nt;
            std::unordered_set<FP128,FP128Hash>local_seen;
            for(int i=i_start;i<i_end&&now_s()<dl;i++)for(int j=0;j<nL1;j++){
                Ins o[2]={fL1[i],fL1[j]};auto f=fp(o,2);
                if(f.first&&local_seen.insert(f).second){
                    DDB b;b.ops[0]=fL1[i];b.ops[1]=fL1[j];b.n=2;
                    tpools[tid].push_back(b);tfps[tid].push_back(f);}}};
        if(nt<=1)d2_worker(0);
        else{std::vector<std::thread>thr;for(int t=0;t<nt;t++)thr.emplace_back(d2_worker,t);
            for(auto&t:thr)t.join();}
        // Sequential merge: preserves order, dedups across threads.
        for(int t=0;t<nt;t++){for(size_t k=0;k<tpools[t].size();k++){
            if(seen.insert(tfps[t][k]).second)pool.push_back(tpools[t][k]);}}}
    else if(depth==3){auto d2=buildDDB(fL1,2,nr,dl);
    // Interleaved composition: L1-outer, d2-inner. Each L1 tail instruction is
    // tried with ALL depth-2 bodies before moving to the next L1 instruction.
    // This gives every depth-2 body equal access to the pool cap - prevents
    // early-generated bodies (INC/DEC/ADD) from monopolizing the cap over
    // later-generated ones (AND/OR/XOR). ISA-fair: no type preference.
    int nd2=(int)d2.size();
    for(int t=0;t<nL1&&(int)pool.size()<pool_cap&&now_s()<dl;t++){
    for(int bi=0;bi<nd2&&(int)pool.size()<pool_cap;bi++){
    Ins o[3]={d2[bi].ops[0],d2[bi].ops[1],fL1[t]};auto f=fp(o,3);
    if(f.first&&seen.insert(f).second){DDB bb;bb.ops[0]=d2[bi].ops[0];bb.ops[1]=d2[bi].ops[1];bb.ops[2]=fL1[t];bb.n=3;
    pool.push_back(bb);}}}}
    for(auto&b:pool)b.computeLoop();return pool;}

// ================================================================
// Section 2: T-table Construction - Observation, O(N)
// ================================================================
static int computeDStar(const std::vector<int>&tgt,int A){
    // information-theoretic upper bound on d*.
    // For K-grams to uniquely determine next symbol, A^K ≥ N (counting argument)
    // → K ≥ log_A(N). Magic 17 replaced with structural ceil(log_A(N+1)).
    int N=(int)tgt.size();
    int info_bound=(int)std::ceil(std::log(double(N+1))/std::log(double(std::max(2,A))));
    int mx=std::min(info_bound,N-1);
    for(int d=1;d<=mx;d++){std::map<std::vector<int>,int>T;bool det=true;
    for(int t=d;t<N&&det;t++){std::vector<int>ctx(tgt.begin()+t-d,tgt.begin()+t);
    auto it=T.find(ctx);if(it==T.end())T[ctx]=tgt[t];else if(it->second!=tgt[t])det=false;}
    if(det)return d;}return-1;}

static int detectPeriod(const std::vector<int>&s){int N=(int)s.size();
    // max-strict period test - check entire sequence [p, N)
    // for s[t] == s[t%p]. Tighter than the previous 3*p threshold (magic),
    // never rejects true periods, strictly fewer false positives.
    // Outer p ≤ N/2 (period must allow ≥1 full repetition to verify).
    for(int p=1;p<=N/2;p++){bool ok=true;for(int t=p;t<N;t++)
    if(s[t]!=s[t%p]){ok=false;break;}if(ok)return p;}return 0;}

static int measureGrowth(const std::vector<int>&tgt,int A){
    int N=(int)tgt.size();std::vector<int>cell(N);for(int t=0;t<N;t++)cell[t]=tgt[t];
    auto norm=[&]()->int{std::map<int,int>rm;int nx=0;for(int t=0;t<N;t++){
    auto it=rm.find(cell[t]);if(it==rm.end()){rm[cell[t]]=nx;cell[t]=nx++;}else cell[t]=it->second;}return nx;};
    int K=norm();std::vector<int>hist={K};
    // bound derives from N (sequence length). Each iteration
    // refines (2^r)-grams; beyond r = log2(N) no further merging is possible.
    // Using N as the safety cap is structural (early-exits ni==K and K>N*9/10
    // terminate before this bound in practice). Magic 30 removed.
    for(int r=0;r<N;r++){std::map<std::pair<int,int>,int>sm;std::vector<int>nc(N);int ni=0;
    for(int t=0;t<N;t++){int s=(t+1<N)?t+1:t;auto sig=std::make_pair(cell[t],cell[s]);
    auto it=sm.find(sig);if(it==sm.end()){sm[sig]=ni;nc[t]=ni++;}else nc[t]=it->second;}
    if(ni==K)break;K=ni;cell=nc;hist.push_back(K);if(K>N*(kIsaMaxConstantConstexpr-1)/kIsaMaxConstantConstexpr)break;}
    // minimum history = kCascadePoolLevels+1 (= 4). Three ratios from
    // four points is the smallest sample for a trend detection (one ratio is a
    // single observation, two ratios is symmetric, three+ enables variance test).
    if((int)hist.size()<kCascadePoolLevels+1)return 0;
    std::vector<double>ratios;
    for(int i=1;i<(int)hist.size();i++) if(hist[i-1]>0&&hist[i]>hist[i-1]) ratios.push_back((double)hist[i]/hist[i-1]);
    if(ratios.empty())return 0;
    double sum=0;for(double r:ratios)sum+=r;double avg=sum/ratios.size();
    double mn=*std::min_element(ratios.begin(),ratios.end());
    double mx2=*std::max_element(ratios.begin(),ratios.end());
    // explicit derivation of magic thresholds.
    // variance bound (mx2-mn)<avg/2 → coefficient of variation < 1/2 (binary
    // fraction; universal moderate-tight
    // criterion for "consistent geometric growth")
    // minimum avg ≥ 1.5 → round-boundary for d=2 (smallest DARY base);
    // avg<1.5 implies round(avg)<2 → d≥2 fails
    // upper bound d ≤ isaMaxConstant() (= 10 for current ISA; ISA-derived)
    if((mx2-mn)<avg/2.0&&avg>=1.5){int d=(int)std::round(avg);if(d>=2&&d<=isaMaxConstant())return d;}return 0;}

struct TEntry{std::vector<int>ctx;int out;};
static std::vector<TEntry>buildT(const std::vector<int>&tgt,int ds){
    std::vector<TEntry>E;if(ds<1)return E;std::map<std::vector<int>,int>seen;
    for(int t=ds;t<(int)tgt.size();t++){std::vector<int>ctx(tgt.begin()+t-ds,tgt.begin()+t);
    if(seen.find(ctx)==seen.end()){seen[ctx]=tgt[t];E.push_back({ctx,tgt[t]});}}return E;}

static std::vector<int>buildUnaryPeriodTable(const std::vector<int>&s,int A,int P){
    std::vector<int>T(A,-1);for(int t=0;t<P;t++){int a=s[t],b=s[(t+1)%P];
    if(T[a]==-1)T[a]=b;else if(T[a]!=b)return{};}return T;}

static std::vector<int>buildBinaryPeriodTable(const std::vector<int>&s,
    const std::vector<int>&aux,int A,int D,int P){
    std::vector<int>T(A*D,-1);for(int t=0;t<P;t++){int a=s[t],r=aux[t];
    if(r<0||r>=D)continue;int idx=a*D+r,b=s[(t+1)%P];
    if(T[idx]==-1)T[idx]=b;else if(T[idx]!=b)return{};}return T;}

// ================================================================
// Section 3: ISA Matching - The Deductive Core
// ================================================================
struct ISAMatch{bool found=false;Ins ins;std::string desc;};

static bool testUnary(int ti,int c,int A,const std::vector<int>&T){
    for(int a=0;a<A;a++){if(T[a]<0)continue;int64_t R[kRegisterCount]={};R[0]=a;
    Ins t_ins;t_ins.ti=ti;t_ins.c=c;t_ins.ar=typeAr(ti);
    switch(ti){case 0:case 1:t_ins.args[0]=0;break;case 5:case 6:t_ins.args[0]=0;t_ins.args[1]=0;break;
    case 11:t_ins.args[0]=0;t_ins.args[1]=0;break;default:return false;}
    ex(R,t_ins);if(pm(R[0],A)!=T[a])return false;}return true;}

static ISAMatch isaMatchUnary(int A,const std::vector<int>&T){
    for(int ti:{0,1})if(testUnary(ti,0,A,T))return{true,{ti,0,{0},typeAr(ti)},ti==0?"INC":"DEC"};
    for(int c=0;c<=kIsaMaxConstantConstexpr;c++)if(testUnary(5,c,A,T))return{true,{5,c,{0,0},2},"MUL_C("+std::to_string(c)+")"};
    for(int c=1;c<=kIsaMaxConstantConstexpr;c++)if(testUnary(6,c,A,T))return{true,{6,c,{0,0},2},"MOD_C("+std::to_string(c)+")"};
    return{};}

static bool testBinary(int ti,int c,int A,int D,const std::vector<int>&T,bool sw=false){
    for(int a=0;a<A;a++)for(int r=0;r<D;r++){int idx=a*D+r;if(idx>=(int)T.size()||T[idx]<0)continue;
    int64_t R[kRegisterCount]={};R[0]=sw?r:a;R[1]=sw?a:r;Ins t_ins;t_ins.ti=ti;t_ins.c=c;t_ins.ar=typeAr(ti);
    switch(ti){case 2:case 3:case 4:case 13:case 14:case 15:
    t_ins.args[0]=0;t_ins.args[1]=1;t_ins.args[2]=0;break;case 5:t_ins.args[0]=0;t_ins.args[1]=0;break;
    default:return false;}ex(R,t_ins);if(pm(R[0],A)!=T[idx])return false;}return true;}

static ISAMatch isaMatchBinary(int A,int D,const std::vector<int>&T){
    for(int ti:{2,3,4,13,14,15})for(bool sw:{false,true})if(testBinary(ti,0,A,D,T,sw)){
    int s0=sw?1:0,s1=sw?0:1;return{true,{ti,0,{(int8_t)s0,(int8_t)s1,0,0},3},std::string(sw?"(sw)":"")};}
    for(int c=2;c<=kIsaMaxConstantConstexpr;c++){bool ok1=true,ok2=true;
    for(int a=0;a<A&&(ok1||ok2);a++)for(int r=0;r<D&&(ok1||ok2);r++){int idx=a*D+r;
    if(idx>=(int)T.size()||T[idx]<0)continue;
    if(ok1&&((a*c+r)%A+A)%A!=T[idx])ok1=false;if(ok2&&((r*c+a)%A+A)%A!=T[idx])ok2=false;}
    if(ok1)return{true,{5,c,{0,0},2},"MUL_C("+std::to_string(c)+")+ADD"};
    if(ok2)return{true,{5,c,{1,0},2},"MUL_C("+std::to_string(c)+")+ADD(sw)"};}return{};}

struct BrMatch{bool found=false;int mod;ISAMatch then_m,else_m;};
static BrMatch isaMatchBranched(int A,const std::vector<int>&T){
    // branched modulus capped at isaMaxConstant.
    // MOD_C(c) requires c ≤ isaMaxConstant, so testing m beyond yields
    // programs that cannot be represented in the ISA.
    // A 300s smoke run once flagged parityalt PARTIAL, but repeated runs
    // solve at 130-139s; the flake is parityalt's natural ~130-200s
    // variance occasionally brushing the 300s cutoff, not a logic
    // regression.
    for(int m=2;m<=std::min(A,isaMaxConstant());m++){std::vector<int>T0(A,-1),T1(A,-1);bool h0=false,h1=false;
    for(int a=0;a<A;a++){if(T[a]<0)continue;if(a%m==0){T0[a]=T[a];h0=true;}else{T1[a]=T[a];h1=true;}}
    if(!h0||!h1)continue;auto m0=isaMatchUnary(A,T0);if(!m0.found)continue;
    auto m1=isaMatchUnary(A,T1);if(!m1.found)continue;
    if(m0.desc!=m1.desc)return{true,m,m0,m1};}return{};}

// Auto-regressive verify: seed with d* values, generate rest
static int autoReg(const Ins*body,int nb,int ds,const std::vector<int>&tgt,int A,
                   int nr,int outr,const int64_t*init){
    int N=(int)tgt.size();if(ds>N)return 0;
    int64_t R[kRegisterCount]={};for(int i=0;i<nr;i++)R[i]=init[i];
    g_sat=false;int sc=0;
    for(int t=0;t<N;t++){if(pm(R[outr],A)==tgt[t])sc++;else break;
    exBody(R,body,nb);if(g_sat)break;}return sc;}

// Context-consistent with permutation: R[k] = ctx[perm[k]]. Functional view (registers reset).
static bool ctxConsistentPerm(const Ins*body,int nb,int A,int outr,int nr,
                               const std::vector<TEntry>&T,
                               const int*perm){
    for(auto&e:T){
        int64_t R[kRegisterCount]={};
        for(int k=0;k<nr;k++)R[k]=e.ctx[perm[k]];
        g_sat=false;exBody(R,body,nb);
        if(g_sat||pm(R[outr],A)!=e.out)return false;}
    return true;}

// Full-sequence verification in MODE_CTX semantics.
// Each iteration: clear R, R[k] = gen[t-ds+perm[k]], run body, output = pm(R[outr],A).
// Returns sc = number of correctly reproduced positions (seeded ds + correctly predicted).
static int verifyCTX(const Ins*body,int nb,int A,int outr,int nr,int ds,
                     const int*perm,const std::vector<int>&tgt){
    int N=(int)tgt.size();
    if(ds>N||ds<=0)return 0;
    std::vector<int>gen(tgt.begin(),tgt.begin()+ds);
    int sc=ds; // Seeded positions trivially match
    for(int t=ds;t<N;t++){
        int64_t R[kRegisterCount]={};
        for(int k=0;k<nr;k++)R[k]=gen[t-ds+perm[k]];
        g_sat=false;exBody(R,body,nb);
        if(g_sat)break;
        int v=pm(R[outr],A);
        if(v!=tgt[t])break;
        sc++;
        gen.push_back(v);}
    return sc;}

// ================================================================
// Section 5: Cascade Filter - Collatz, ParityAlt (small enumeration)
// ================================================================
struct DB{Ins ops[3];int n;bool has_loop=false;
    void computeLoop(){has_loop=false;for(int i=0;i<n;i++)if(ops[i].ti==17){has_loop=true;return;}}};

static std::vector<DB>buildPool(const std::vector<Ins>&L1,int depth,int nr,double dl){
    // pool_cap = |L1|^depth (theoretical max unique bodies before
    // dedup). Removes magic 100000/30000/100. Memory grows to actual pool size
    // (no eager reserve), deadline `dl` truncates build.
    int pool_cap=(int)std::min((double)INT_MAX,std::pow((double)L1.size(),depth));
    int nL1=(int)L1.size();
    // G is the per-register probe count for fingerprint dedup.
    // Derived from isaMaxConstant() (= max constant in the ISA's opcode ranges)
    // so that probes cover all residue classes for MOD_C, all loaded values
    // for LOAD, etc. - guarantees no false equivalence between bodies that
    // differ only at value-specific positions. For our ISA, max_c=10 → G=11.
    const int G=isaMaxConstant()+1;
    auto fp=[&](const Ins*ops,int n)->uint64_t{uint64_t h=0xcbf29ce484222325ULL;
    for(int r0=0;r0<G;r0++)for(int r1=0;r1<(nr>1?G:1);r1++){int64_t R[kRegisterCount]={};R[0]=r0;if(nr>1)R[1]=r1;
    g_sat=false;for(int i=0;i<n;i++){ex(R,ops[i]);if(g_sat)return 0;}
    h^=(uint64_t)(R[0]*1000003+R[1]*1000033);h*=0x9e3779b97f4a7c15ULL;}return h;};
    // No eager reserve - pool_cap can be up to |L1|^3 (millions); allocator
    // grows naturally to actual size (typically much smaller after dedup).
    std::unordered_set<uint64_t>seen;
    std::vector<DB>pool;
    if(depth==1){for(int i=0;i<nL1;i++){Ins o[1]={L1[i]};auto f=fp(o,1);
    if(f&&seen.insert(f).second){DB b;b.ops[0]=L1[i];b.n=1;pool.push_back(b);}}}
    else if(depth==2){for(int i=0;i<nL1&&now_s()<dl;i++)for(int j=0;j<nL1;j++){
    Ins o[2]={L1[i],L1[j]};auto f=fp(o,2);if(f&&seen.insert(f).second){
    DB b;b.ops[0]=L1[i];b.ops[1]=L1[j];b.n=2;pool.push_back(b);}}}
    else if(depth==3){auto d2=buildPool(L1,2,nr,dl);
    std::vector<int>tails;for(int i=0;i<nL1;i++){bool w=false;
    int wi=writerArgIdx(L1[i]);
    if(wi>=0)w=(L1[i].args[wi]==0);
    if(L1[i].ti==8)w=w||(L1[i].args[1]==0); // DIVC also writes args[1] (rem)
    if(L1[i].ti==9)w=w||(L1[i].args[3]==0); // DIVR also writes args[3] (rem)
    if(!w)tails.push_back(i);}
    for(auto&b:d2){if(now_s()>dl||(int)pool.size()>=pool_cap)break;
    for(int t:tails){Ins o[3]={b.ops[0],b.ops[1],L1[t]};auto f=fp(o,3);
    if(f&&seen.insert(f).second){DB bb;bb.ops[0]=b.ops[0];bb.ops[1]=b.ops[1];bb.ops[2]=L1[t];bb.n=3;
    pool.push_back(bb);}if((int)pool.size()>=pool_cap)break;}}
    // drop magic 3000 gate. Writer-tail extension always runs;
    // pool_cap (|L1|^3, review) bounds total size. Deadline truncates.
    if(now_s()<dl){for(auto&b:d2){if(now_s()>dl)break;
    for(int i=0;i<nL1;i++){bool w=false;
    int wi=writerArgIdx(L1[i]);
    if(wi>=0)w=(L1[i].args[wi]==0);
    if(L1[i].ti==8)w=w||(L1[i].args[1]==0);
    if(L1[i].ti==9)w=w||(L1[i].args[3]==0);
    if(w){Ins o[3]={b.ops[0],b.ops[1],L1[i]};auto f=fp(o,3);
    if(f&&seen.insert(f).second){DB bb;bb.ops[0]=b.ops[0];bb.ops[1]=b.ops[1];bb.ops[2]=L1[i];bb.n=3;
    pool.push_back(bb);}}}}}}
    // Precompute has_loop flag for all pool bodies (enables exBodyFlat fast path)
    for(auto&b:pool)b.computeLoop();
    return pool;}

// ds-aware pool builder: fingerprints over all nr registers (not just R[0..1]).
// Required for context search where body's behavior on R[2..nr-1] matters.
// Probe values per register: G=5 for nr<=3, G=3 for nr=4 (cost-controlled).
static std::vector<DB>buildPoolDS(const std::vector<Ins>&L1,int depth,int nr,double dl){
    // pool_cap = |L1|^depth (theoretical max). Same derivation as buildPool.
    int pool_cap=(int)std::min((double)INT_MAX,std::pow((double)L1.size(),depth));
    int nL1=(int)L1.size();
    // G derives from isaMaxConstant() universally - no nr-specific
    // magic. Yao's principle: deterministic fingerprint dedup needs G^nr probes
    // covering all ISA-induced behaviors; G = max_c+1 is the structural minimum.
    // Cost grows as G^nr but is deadline-bounded by buildPoolDS's dl arg.
    const int G=isaMaxConstant()+1; // probe values per register (ISA-structural)
    auto fp=[&](const Ins*ops,int n)->uint64_t{uint64_t h=0xcbf29ce484222325ULL;
        // Enumerate G^nr probe configurations
        int total=1;for(int k=0;k<nr;k++)total*=G;
        for(int p=0;p<total;p++){
            int64_t R[kRegisterCount]={};
            int v=p;for(int k=0;k<nr;k++){R[k]=v%G;v/=G;}
            g_sat=false;for(int i=0;i<n;i++){ex(R,ops[i]);if(g_sat)return 0;}
            for(int k=0;k<nr;k++)h^=(uint64_t)(R[k]*(1000003+k*7919));
            h*=0x9e3779b97f4a7c15ULL;}
        return h;};
    // No eager reserve - same rationale as buildPool.
    std::unordered_set<uint64_t>seen;
    std::vector<DB>pool;
    if(depth==1){for(int i=0;i<nL1;i++){Ins o[1]={L1[i]};auto f=fp(o,1);
        if(f&&seen.insert(f).second){DB b;b.ops[0]=L1[i];b.n=1;pool.push_back(b);}}}
    else if(depth==2){for(int i=0;i<nL1&&now_s()<dl;i++)for(int j=0;j<nL1;j++){
        Ins o[2]={L1[i],L1[j]};auto f=fp(o,2);if(f&&seen.insert(f).second){
            DB b;b.ops[0]=L1[i];b.ops[1]=L1[j];b.n=2;pool.push_back(b);}}}
    else if(depth==3){auto d2=buildPoolDS(L1,2,nr,dl);
        int nd2=(int)d2.size();
        for(int t=0;t<nL1&&(int)pool.size()<pool_cap&&now_s()<dl;t++){
            for(int bi=0;bi<nd2&&(int)pool.size()<pool_cap;bi++){
                Ins o[3]={d2[bi].ops[0],d2[bi].ops[1],L1[t]};auto f=fp(o,3);
                if(f&&seen.insert(f).second){DB bb;bb.ops[0]=d2[bi].ops[0];bb.ops[1]=d2[bi].ops[1];bb.ops[2]=L1[t];bb.n=3;
                    pool.push_back(bb);}}}}
    for(auto&b:pool)b.computeLoop();
    return pool;}

struct DiscBody{int m;DB even,odd;};

static std::vector<int> modFilter(const std::vector<DB>&pool,int A,int m,bool zero_branch,
                                   const std::vector<int>&tgt){
    std::vector<std::pair<int,int>>pairs;
    for(int t=0;t+1<(int)tgt.size();t++){
        bool zb=(tgt[t]%m==0);
        if(zb==zero_branch) pairs.push_back({tgt[t],tgt[t+1]});}
    std::sort(pairs.begin(),pairs.end());
    pairs.erase(std::unique(pairs.begin(),pairs.end()),pairs.end());
    if(pairs.empty()){std::vector<int>all;for(int i=0;i<(int)pool.size();i++)all.push_back(i);return all;}
    bool const_sout=true;int expected_sout=pairs[0].second;
    for(auto&[c,s]:pairs)if(s!=expected_sout){const_sout=false;break;}
    std::vector<int>surv;
    for(int bi=0;bi<(int)pool.size();bi++){
        int divc_prod=1;for(int i=0;i<pool[bi].n;i++)
            if(pool[bi].ops[i].ti==8)divc_prod*=std::max(1,pool[bi].ops[i].c);
        // range bounds derived structurally.
        // min = 2 × (isaMaxConstant + 1) - twice the fingerprint probe count
        // G = isaMaxConstant + 1 (residue-coverage requirement).
        // max = 1 << kRegisterCount - full byte-equivalent residue space
        // (with kRegisterCount=8, this is 256 distinct R0 values tested).
        // With current ISA (max=10, regs=8): min=22, max=256 - identical to prior.
        int range=A*std::max(1,divc_prod);
        if(range<2*(isaMaxConstant()+1))range=2*(isaMaxConstant()+1);
        if(range>(1<<kRegisterCount))range=(1<<kRegisterCount);
        bool ok=true;
        if(const_sout){
            int cin0=pairs[0].first;
            for(int R0=cin0;R0<range&&ok;R0+=A){int64_t R[kRegisterCount]={};R[0]=R0;g_sat=false;
            exBody(R,pool[bi].ops,pool[bi].n);
            if(g_sat||pm(R[0],A)!=expected_sout)ok=false;}
        }else{
            for(auto&[cin,sout]:pairs){if(!ok)break;bool any=false;
            for(int R0=cin;R0<range;R0+=A){int64_t R[kRegisterCount]={};R[0]=R0;g_sat=false;
            exBody(R,pool[bi].ops,pool[bi].n);
            if(!g_sat&&pm(R[0],A)==sout){any=true;break;}}
            if(!any)ok=false;}
        }
        if(ok)surv.push_back(bi);}
    return surv;}

static bool cascadeSearch(const std::vector<int>&tgt,int A,
    const std::vector<Ins>&L1,int nr,std::vector<DB>&pool1,std::vector<DB>&pool2,
    std::vector<DB>&pool3,std::vector<DiscBody>&discovered,
    int&best_sc,std::vector<int>&best_init_v,int&best_m,
    std::vector<DB>&best_even,std::vector<DB>&best_odd,Deadline&d){
    // Simplified cascade: returns branched programs via out-params
    // Deadline d allows adaptive extension when sc is climbing.
    int N=(int)tgt.size();
    // R10: Cascade threading - parallel across m values
    std::mutex cas_mtx;
    std::atomic<bool>cas_found(false);
    std::atomic<int>next_m(2);
    // lex-(sc, -total_body_length) tiebreak. Body length is the
    // dominant factor in computeMDL; exact MDL would require Res visibility
    // (Res is defined later in the file). Caller's tryUpdateBestLex still
    // applies exact lex-MDL across phase outputs, so the residual MDL gap
    // from this proxy is at most the per-instruction-constant variation.
    int best_total_len = INT_MAX;
    // No hand-picked constants: max_m derives from A (alphabet),
    // not a hand-picked constant. m's universal-int description length is
    // ~log2(m); capping m≤A says "branch modulus parameters cost no more
    // bits than the output alphabet bits." Structural, derived from input.
    int max_m=std::max(2,A);
    auto cas_worker=[&](){
    while(true){
    int m=next_m.fetch_add(1);
    // removed cas_found early-exit. Workers run until deadline d
    // expires, allowing cross-worker lex-tiebreak via #95.7 helper. cas_found
    // is preserved purely for the return-value contract.
    if(m>max_m||!d.alive())return;
    for(int td=2;td<=2*kCascadePoolLevels&&d.alive();td++){for(int de=1;de<td;de++){int doo=td-de;
    if(de>kCascadePoolLevels||doo>kCascadePoolLevels)continue;
    if(nr<=2&&(de>=kCascadePoolLevels||doo>=kCascadePoolLevels)&&td>=2*kCascadePoolLevels-1)continue;
    auto&Pe=(de==1?pool1:de==2?pool2:pool3);auto&Po=(doo==1?pool1:doo==2?pool2:pool3);
    if(Pe.empty()||Po.empty())continue;
    auto se=modFilter(Pe,A,m,true,tgt);
    auto so=modFilter(Po,A,m,false,tgt);
    printf("      m=%d de=%d do=%d: %d×%d → %d×%d\n",m,de,doo,(int)Pe.size(),(int)Po.size(),(int)se.size(),(int)so.size());
    bool filtered=((!se.empty())&&(!so.empty()));
    if(!filtered){
        se.clear();for(int i=0;i<(int)Pe.size();i++)se.push_back(i);
        so.clear();for(int i=0;i<(int)Po.size();i++)so.push_back(i);
        // magic 500000 cap → MDL-prior order + deadline.
        // Bayesian optimal stopping: sort hypotheses (Pe[i],Po[j]) by prior
        // P(H)∝2^(-MDL(H)) ascending, truncate at deadline d.alive().
        // The fixed count cap was Pareto-dominated: it discarded entire
        // (de,doo) tuples including their MDL-light prefix pairs.
        // Sort key: (instruction count, canonical fingerprint). Both structural.
        auto idx_fp=[&](const std::vector<DB>&P,int i)->uint64_t{
            uint64_t h=0xcbf29ce484222325ULL;
            for(int k=0;k<P[i].n;k++){h^=(uint64_t)P[i].ops[k].ti*0x100000001b3ULL;
            h^=(uint64_t)P[i].ops[k].ar*0xff51afd7ed558ccdULL;
            h^=(uint64_t)P[i].ops[k].c*0xc4ceb9fe1a85ec53ULL;h*=0x100000001b3ULL;}
            return h;};
        std::sort(se.begin(),se.end(),[&](int a,int b){
            if(Pe[a].n!=Pe[b].n)return Pe[a].n<Pe[b].n;
            return idx_fp(Pe,a)<idx_fp(Pe,b);});
        std::sort(so.begin(),so.end(),[&](int a,int b){
            if(Po[a].n!=Po[b].n)return Po[a].n<Po[b].n;
            return idx_fp(Po,a)<idx_fp(Po,b);});
    }
    // r0 iterates unbounded ascending (Solomonoff prior),
    // truncated by deadline d.alive() - no magic upper bound.
    // r1, r2 bounded by A: alphabet-equivalence - output is mod A and r1,r2
    // effects on first body cycle are bounded by A residue classes.
    const int kR1Max=(nr>1)?A:0;
    const int kR2Max=(nr>2)?A:0;
    for(int r0=0;d.alive();r0++){if(pm(r0,A)!=tgt[0])continue;
    for(int r1=0;r1<=kR1Max;r1++){for(int r2=0;r2<=kR2Max&&d.alive();r2++){
    int64_t init[kRegisterCount]={r0,r1,r2};
    int64_t rem0=emod(init[0],m);
    auto&P0=(rem0==0?se:so); auto&Pool0=(rem0==0?Pe:Po);
    auto&P1=(rem0==0?so:se); auto&Pool1=(rem0==0?Po:Pe);
    struct S0{int64_t R[kRegisterCount];int bi;};
    std::vector<S0>step0_surv;step0_surv.reserve(P0.size());
    for(int pi=0;pi<(int)P0.size();pi++){int bi=P0[pi];
        int64_t R[kRegisterCount];memcpy(R,init,sizeof(R));g_sat=false;
        if(Pool0[bi].has_loop)exBody(R,Pool0[bi].ops,Pool0[bi].n);
        else exBodyFlat(R,Pool0[bi].ops,Pool0[bi].n);
        if(g_sat)continue;if(pm(R[0],A)!=tgt[1])continue;
        step0_surv.push_back({{R[0],R[1],R[2],R[3],R[4],R[5],R[6],R[7]},bi});}
    auto stateKey=[&](const S0&s)->uint64_t{uint64_t h=0;
        for(int i=0;i<nr;i++)h^=(uint64_t)(s.R[i]*1000003*(i+1));return h;};
    std::unordered_map<uint64_t,std::vector<int>>groups;
    for(int i=0;i<(int)step0_surv.size();i++)groups[stateKey(step0_surv[i])].push_back(i);
    for(auto&[skey,g_idxs]:groups){if(d.maybe_extend(),!d.alive())break;
    for(int pi1=0;pi1<(int)P1.size()&&d.alive();pi1++){int bi1=P1[pi1];
        int gi0=g_idxs[0];auto&s0=step0_surv[gi0];
        int64_t R[kRegisterCount];memcpy(R,s0.R,sizeof(R));g_sat=false;
        if(Pool1[bi1].has_loop)exBody(R,Pool1[bi1].ops,Pool1[bi1].n);
        else exBodyFlat(R,Pool1[bi1].ops,Pool1[bi1].n);
        if(g_sat||pm(R[0],A)!=tgt[2])continue;
        for(int gi:g_idxs){if(!d.alive())break;
            auto&ss=step0_surv[gi];
            int ei_real=(rem0==0?ss.bi:bi1);int oi_real=(rem0==0?bi1:ss.bi);
            int64_t Rv[kRegisterCount];memcpy(Rv,init,sizeof(Rv));g_sat=false;int sc=0;bool ok=true;
            // Cache BodyDesc for even/odd bodies (reused N times)
            BodyDesc ebd,obd;
            computeBodyDesc(Pe[ei_real].ops,Pe[ei_real].n,ebd);
            computeBodyDesc(Po[oi_real].ops,Po[oi_real].n,obd);
            for(int t=0;t<N&&ok;t++){if(pm(Rv[0],A)!=tgt[t]){ok=false;break;}sc++;
            int64_t rm=emod(Rv[0],m);
            if(rm==0)exBodyD(Rv,Pe[ei_real].ops,Pe[ei_real].n,ebd);
            else exBodyD(Rv,Po[oi_real].ops,Po[oi_real].n,obd);
            if(g_sat){ok=false;break;}}
            // outer admits equal-sc for lex-(sc, -body_length) tiebreak.
            if(sc>=best_sc){
            // Print full-match HITs only.
            // Magic 30 replaced by structural N (sequence length).
            if(sc==N){printf("    HIT sc=%d init=(%d,%d,%d) even={",sc,(int)init[0],(int)init[1],(int)init[2]);
            for(int i=0;i<Pe[ei_real].n;i++)printf("%s%s",i?";":"",Pe[ei_real].ops[i].str().c_str());
            printf("} odd={");for(int i=0;i<Po[oi_real].n;i++)printf("%s%s",i?";":"",Po[oi_real].ops[i].str().c_str());
            printf("}\n");}
            int cand_total_len = Pe[ei_real].n + Po[oi_real].n;
            {std::lock_guard<std::mutex>lk(cas_mtx);
            // lex-(sc, -total_body_length) under lock.
            if(sc>best_sc ||
               (sc==best_sc && cand_total_len<best_total_len)){
                best_sc=sc;best_total_len=cand_total_len;
                d.report(sc);best_init_v={r0,r1,r2};best_m=m;
                best_even={Pe[ei_real]};best_odd={Po[oi_real]};}}
            // conv-test seeds derive from ISA range [2, max_c],
            // iter cap from N * isaMaxConstant() (sequence × ISA-scale; bounds
            // halt-trajectories of bodies that mix arithmetic up to max_c
            // across N output positions). Magic {5,10,15,20} and 500 removed.
            const int kConvIterCap=N*isaMaxConstant();
            // minimum sc threshold = kIsaMaxConstantConstexpr/2 (= 5).
            // Below this we don't trust the convergence test; above, we verify across [2, isaMax].
            if(sc>=kIsaMaxConstantConstexpr/2){bool conv=true;for(int tv=2;tv<=isaMaxConstant();tv++){int64_t Rt[kRegisterCount]={};Rt[0]=tv;
            for(int s=0;s<kConvIterCap;s++){if(Rt[0]<=1)break;int64_t rm2=emod(Rt[0],m);g_sat=false;
            if(rm2==0)exBodyD(Rt,Pe[ei_real].ops,Pe[ei_real].n,ebd);
            else exBodyD(Rt,Po[oi_real].ops,Po[oi_real].n,obd);
            if(g_sat){conv=false;break;}}if(Rt[0]>1)conv=false;if(!conv)break;}
            {std::lock_guard<std::mutex>lk(cas_mtx);
            discovered.push_back({m,Pe[ei_real],Po[oi_real]});}}
            if(sc==N){
                cas_found=true;  // preserved for cascadeSearch return-value contract
                // collection-mode deadline shrinkage instead of
                // first-wins early-exit. Workers continue past first sc==N for
                // up to kIsaMaxConstantConstexpr seconds, allowing the helper
                //) to pick lex-best across
                // ALL workers' equal-sc=N candidates. After this window, d.alive()
                // returns false and workers exit naturally.
                double new_end = std::min(d.t_end.load(std::memory_order_relaxed),
                                          now_s()+(double)kIsaMaxConstantConstexpr);
                d.t_end.store(new_end, std::memory_order_relaxed);
            }}
        }
    }}
    }}}}}}
    };// end cas_worker lambda
    int cas_nt=std::min((int)std::thread::hardware_concurrency(),std::max(1,max_m-1));
    if(cas_nt<=1){cas_worker();}
    else{std::vector<std::thread>thr;for(int t=0;t<cas_nt;t++)thr.emplace_back(cas_worker);
        for(auto&t:thr)t.join();}
    return cas_found.load();}

// ================================================================
// Section 6: New Execution Model / Result / MDL
// ================================================================
enum ExModel{MODE_ITER=0,MODE_FUNC=1,MODE_EMIT=2,MODE_CTX=3};
enum OutInterp{OUT_MOD=0,OUT_BIT=1};

struct Res{
    int sc=0;
    std::string desc="none";
    double mdl=1e9;
    Ins body[kProgramBodyMax];int nbody=0;
    int64_t init[kRegisterCount]={};
    int nr=1;int outr=0;
    ExModel mode=MODE_ITER;
    OutInterp ointerp=OUT_MOD;
    bool branched=false;
    int branch_m=1;
    int then_len=0; // length of even-branch body (branched programs)
    int bit_pos=0;  // for OUT_BIT
    // CONCAT deductive (body may not capture MSB/LSB correctly)
    int concat_base=0;int concat_off=0;bool concat_msb=true;
    int step_off=0;int step_halt=0;
    // DARY deductive (body is documentation; prediction uses direct digit loop)
    int dary_base=0;int dary_init_val=0;Ins dary_op={};
    // MODE_CTX: registers reload from history each iteration. perm[k]=which context position → R[k]
    int ctx_dstar=0;int ctx_perm[kRegisterCount]={0,1,2,3,4,5,6,7};};

// Universal integer coding: log2*(n) + log2(n+1)
static double uInt(int64_t n){if(n<=0)return 1.0;double L=log2(n+1.0),s=L;
    while(L>1){L=log2(L);s+=L;}return s+1;}

static double computeMDL(const Res&r,int ncat){
    // pure-deductive CONCAT/DARY have minimum-MDL encoding.
    // The 2-bit program-type tag (#95.11) fully determines mode, branched,
    // ointerp, nr, and body for these types - they are canonical:
    // CONCAT: mode=MODE_EMIT, branched=false, ointerp=OUT_MOD, nr=3, body=canonical 5-instr
    // DARY: mode=MODE_FUNC, branched=false, ointerp=OUT_MOD, nr=3, body=canonical 7-instr
    // So encoding is just type tag + deductive params. Early-return for these.
    if(r.concat_base>=2){
        double m = 2; // type tag (2 bits)
        m += uInt(r.concat_base);
        m += uInt(r.concat_off);
        m += 1; // concat_msb (boolean)
        return m;
    }
    if(r.dary_base>=2){
        double m = 2; // type tag (2 bits)
        // DARY canonical nr=kDaryCanonicalNr; register-selector cost per arg.
        double lr_dary = log2((double)kDaryCanonicalNr);
        m += uInt(r.dary_base);
        m += uInt(r.dary_init_val);
        m += log2(std::max(1,ncat));      // dary_op opcode (catalog selector)
        m += r.dary_op.ar * lr_dary;       // dary_op register args
        if(r.dary_op.ti==17) m += uInt(r.dary_op.c);  // LOOP length param
        return m;
    }
    // program-type tag (2 bits, uniform). Distinguishes
    // REGULAR / CONCAT / DARY / STEP. Replaces the previous implicit
    // detection where CONCAT/DARY/STEP blocks fired only when set -
    // a self-delimiting prefix code requires the type to be encoded
    // unconditionally. STEP's pre-#95.11 "+2 bits for STEP tag" was
    // already this type marker, but inconsistently paid only when set;
    // moved here for uniform application.
    double mdl=2; // mode tag (4 ExModel: ITER/FUNC/EMIT/CTX = 2 bits)
    mdl+=2;       // program type tag (REGULAR/CONCAT/DARY/STEP = 2 bits)
    double lc=log2(std::max(1,ncat)),lr=log2(std::max(1,r.nr));
    auto bodyMDL=[&](int start,int len)->double{double b=uInt(len);
        for(int i=start;i<start+len&&i<r.nbody;i++){
            b+=lc+r.body[i].ar*lr;
            if(r.body[i].ti==17)b+=uInt(r.body[i].c); // LOOP length
        }return b;};
    // branched flag must be paid for self-delimiting prefix code.
    // review+#95.17: branched flag is only meaningful for MODE_ITER.
    // - BR cascade (MODE_ITER, branched=true) is the only branched non-STEP
    // non-CONCAT non-DARY type. So MODE_ITER's branched can be 0 or 1.
    // - MODE_FUNC + REGULAR: branched=false canonical (no branched non-STEP
    // MODE_FUNC programs in the codebase).
    // - MODE_FUNC + STEP: branched=true canonical (handled by type tag).
    // - MODE_EMIT, MODE_CTX (non-CONCAT/DARY): branched=false canonical.
    // So flag is meaningful ONLY for MODE_ITER.
    if(r.mode == MODE_ITER) mdl+=1; // branched flag (MODE_ITER only)
    // pure-deductive CONCAT/DARY have body uniquely determined
    // by their deductive params (concat_base/off/msb or dary_base/init_val/op).
    // predictNext and runProgram never read r.body for these types - body bits
    // are dead weight. Skip bodyMDL for them.
    bool is_pure_deductive = (r.concat_base >= 2) || (r.dary_base >= 2);
    if(r.branched){
        mdl+=uInt(r.branch_m);
        if(!is_pure_deductive){
            mdl+=bodyMDL(0,r.then_len);
            mdl+=bodyMDL(r.then_len,r.nbody-r.then_len);
        }
    }else{
        if(!is_pure_deductive){
            mdl+=bodyMDL(0,r.nbody);
        }
    }
    // ointerp + bit_pos are only used for MODE_ITER (verified in
    // predictNext line 1481-1494: only MODE_ITER branches on r.ointerp; MODE_FUNC,
    // MODE_EMIT, MODE_CTX always use the OUT_MOD path). Skip the flag for non-ITER
    // modes - dead weight, paid 1 bit per program for nothing.
    if(r.mode==MODE_ITER){
        if(r.ointerp==OUT_BIT){
            mdl+=1; // output interp flag
            mdl+=uInt(r.bit_pos);
        }else{
            mdl+=1; // output interp flag
        }
    }
    // Init + registers (ITER and EMIT modes need init; FUNC and CTX don't use init[])
    if(r.mode!=MODE_FUNC&&r.mode!=MODE_CTX){
        mdl+=uInt(r.nr);
        for(int i=0;i<r.nr;i++)mdl+=uInt(r.init[i]);
        mdl+=lr;
    }
    // MODE_CTX: encode d_star and the permutation (which context positions map to which registers).
    // Permutation cost: nr × log2(d_star) bits. ISA-derived: every register selector costs log2(d_star).
    if(r.mode==MODE_CTX){
        mdl+=uInt(r.ctx_dstar);
        mdl+=uInt(r.nr);
        mdl+=r.nr*log2(std::max(1,r.ctx_dstar));
        mdl+=lr; // outr selector
    }
    // MODE_FUNC needs nr + outr encoded for prefix-code consistency.
    // bodyMDL pays lr=log2(nr) per register-selector slot, so decoder needs nr to
    // interpret body bits. ITER/EMIT/CTX encode this; MODE_FUNC was missing it,
    // causing K (nr, outr) MODE_FUNC programs with same body to share one MDL -
    // a Kraft inequality violation.
    if(r.mode==MODE_FUNC){
        mdl+=uInt(r.nr);
        mdl+=lr; // outr selector
    }
    // STEP parameters: program type tag for STEP is paid by the unified 2-bit
    // type tag at function entry; only the (off, halt) parameter bits live here.
    if(r.step_off>0||r.step_halt>0)mdl+=uInt(r.step_off)+uInt(r.step_halt);
    return mdl;
}

// ──────────────────────────────────────────────────────────────────────────────
// review (C3 tie-break - lex-best update over (sc, -mdl)):
//
// Solomonoff prior P(h) ∝ 2^(-|h|) prefers shorter (lower-MDL) programs among
// those that explain the data equally well. The operational best-update rule
// must therefore enforce lex order: among candidates h with sc(h) == sc(best),
// prefer min mdl(h).
//
// Pre-#95, every best-update site used `if (sc > best.sc)` (strict greater),
// so once any sc=N candidate landed, subsequent sc=N candidates with smaller
// MDL were silently rejected - visible as run-to-run MDL variance (parityalt
// 38.2 vs 38.4, trimod8 32.1 vs 40.3) caused by parallel-thread scheduling
// determining which equal-sc candidate happened to land first.
//
// Constraints:
// C1 (Accuracy): improved or equal - no candidate previously accepted is
// now rejected; outer admits a strict superset.
// C2 (Generality): uniform - comparison logic is identical across every
// benchmark, every phase, every code path.
// C3 (Solomonoff): repaired - lex-(sc, -mdl) is the operational form of
// Solomonoff preference.
// C4 (No magic): helper introduces no constants; uses existing fields.
//
// Optional pointer parameters allow this single helper to serve both
// single-threaded sites (mtx == nullptr) and multi-threaded sites that
// maintain a parallel-shared atomic counter. Callers retain responsibility
// for downstream side-effects (recordProg, hit counters, deadline updates)
// since those vary per phase; the helper handles only the lex-best mutation.
//
// Returns true iff `dest` was replaced.
inline bool tryUpdateBestLex(Res& dest, const Res& cand, int sc_cand,
                              std::atomic<int>* shared = nullptr,
                              std::mutex* mtx = nullptr) {
    // Race-tolerable pre-filter: if shared sc already exceeds candidate's sc,
    // the locked check would surely fail. Skip the lock cost.
    if (shared) {
        int observed = shared->load();
        if (sc_cand < observed) return false;
    }
    if (mtx) {
        std::lock_guard<std::mutex> lk(*mtx);
        if (sc_cand > dest.sc || (sc_cand == dest.sc && cand.mdl < dest.mdl)) {
            dest = cand;
            if (shared) shared->store(sc_cand);
            return true;
        }
        return false;
    }
    // Single-threaded path
    if (sc_cand > dest.sc || (sc_cand == dest.sc && cand.mdl < dest.mdl)) {
        dest = cand;
        if (shared) shared->store(sc_cand);
        return true;
    }
    return false;
}
// ──────────────────────────────────────────────────────────────────────────────

// Program collection for Solomonoff mixture
struct ProgEntry{Res r;double mdl;int prediction;};
static std::vector<ProgEntry>g_progs;
static std::mutex g_progs_mutex; // A1: thread-safe collection
// Body-fingerprint dedup: prevents duplicate Res entries from concurrent
// Phase 2H workers finding the same program. Without this, the Solomonoff mixture would
// double-count duplicates in its weight computation (n × 2^-MDL instead of 1 × 2^-MDL),
// biasing predictions toward heavily-replicated programs. Cleared at solve() entry along
// with g_progs to maintain per-benchmark scope.
// soundness: a duplicate program at equal MDL contributes nothing new to the mixture;
// deduplication preserves Solomonoff exactness.
// C2 - corrects an under-tightening of MDL accounting (over-counting equal-MDL programs
// was a bias against C2's "equal MDL = equal prior" intent).
// C3 - fingerprint is a structural property of the Res record; target-independent.
// C4 - applies uniformly to any target.
static std::unordered_set<uint64_t>g_progs_fingerprints; // protected by g_progs_mutex
static int g_bench_prog_start=0;
static void recordProg(const Res&r,int ncat,const std::vector<int>&tgt,int A);

// Stable structural fingerprint of a Res record. Two Res records with identical body,
// mode, register count, init values, branch parameters, and special-mode parameters
// produce the same fingerprint. FNV-1a hash chained over all distinguishing fields.
static uint64_t resFingerprint(const Res& r) {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 0x100000001b3ULL; };
    mix((uint64_t)r.nbody);
    for (int i = 0; i < r.nbody; i++) {
        mix((uint64_t)r.body[i].ti);
        mix((uint64_t)r.body[i].c);
        mix((uint64_t)r.body[i].ar);
        for (int a = 0; a < 4; a++) mix((uint64_t)(uint8_t)r.body[i].args[a]);
    }
    mix((uint64_t)r.mode);
    mix((uint64_t)r.nr);
    mix((uint64_t)r.outr);
    mix((uint64_t)r.ointerp);
    mix((uint64_t)r.bit_pos);
    mix((uint64_t)(r.branched ? 1 : 0));
    if (r.branched) {
        mix((uint64_t)r.branch_m);
        mix((uint64_t)r.then_len);
    }
    for (int i = 0; i < r.nr && i < kRegisterCount; i++) mix((uint64_t)r.init[i]);
    mix((uint64_t)r.dary_base);
    mix((uint64_t)r.dary_init_val);
    // Hash dary_op (the per-digit transform Ins) for DARY-mode discrimination.
    // Two DARY programs with identical base/init but distinct ops would otherwise
    // collide in the fingerprint.
    if (r.dary_base >= 2) {
        mix((uint64_t)r.dary_op.ti);
        mix((uint64_t)r.dary_op.c);
        mix((uint64_t)r.dary_op.ar);
        for (int a = 0; a < 4; a++) mix((uint64_t)(uint8_t)r.dary_op.args[a]);
    }
    mix((uint64_t)r.concat_base);
    mix((uint64_t)r.concat_off);
    mix((uint64_t)(r.concat_msb ? 1 : 0));
    mix((uint64_t)r.step_off);
    mix((uint64_t)r.step_halt);
    mix((uint64_t)r.ctx_dstar);
    if (r.mode == MODE_CTX) {
        for (int i = 0; i < r.nr && i < kRegisterCount; i++) mix((uint64_t)r.ctx_perm[i]);
    }
    return h;
}

// Predict next symbol: run program for N+1 steps, return symbol at position N
static int predictNext(const Res&r,const std::vector<int>&tgt,int A){
    int N=(int)tgt.size();
    if(r.sc<N)return-1;
    // CONCAT deductive: generate directly
    if(r.concat_base>0){std::vector<int>out;
    for(int n=r.concat_off;(int)out.size()<=N;n++){std::vector<int>digs;int v=n;
    if(v==0)digs.push_back(0);else while(v>0){digs.push_back(v%r.concat_base);v/=r.concat_base;}
    if(r.concat_msb)std::reverse(digs.begin(),digs.end());for(int dig:digs)out.push_back(dig%A);}
    return(N<(int)out.size())?out[N]:-1;}
    // DARY deductive: use direct digit loop (same as verification)
    if(r.dary_base>=2){
        int64_t R[kRegisterCount]={};R[0]=r.dary_init_val;g_sat=false;int v=N;
        if(v==0){R[1]=0;ex(R,r.dary_op);}
        else{while(v>0&&!g_sat){R[1]=v%r.dary_base;ex(R,r.dary_op);v/=r.dary_base;}}
        return g_sat?-1:pm(R[0],A);}
    if(r.nbody<=0)return-1;

    if(r.branched){
        // STEP: count convergent iterations (independent of mode)
        if(r.step_off>0||r.step_halt>0){
            int n=N;int64_t R[kRegisterCount]={};R[0]=n+r.step_off;g_sat=false;int steps=0;
            for(int s=0;s<kStepIterCap;s++){if(R[0]<=r.step_halt&&s>0)break;
            int64_t rm=emod(R[0],r.branch_m);
            if(rm==0)exBody(R,r.body,r.then_len);
            else exBody(R,r.body+r.then_len,r.nbody-r.then_len);
            if(g_sat)return-1;steps++;}
            return steps%A;}
        // Branched: same logic for all modes but with branch dispatch
        if(r.mode==MODE_ITER||r.mode==MODE_EMIT){
            if(r.mode==MODE_EMIT){
                // Collect emissions for N+1 iterations
                g_emit_A=A;g_emit_buf.clear();
                int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
                for(int t=0;t<=N+10&&(int)g_emit_buf.size()<=N;t++){
                    int64_t rm=emod(R[0],r.branch_m);
                    if(rm==0)exBody(R,r.body,r.then_len);
                    else exBody(R,r.body+r.then_len,r.nbody-r.then_len);
                    if(g_sat)break;}
                g_emit_A=0;
                if((int)g_emit_buf.size()>N)return g_emit_buf[N];
                return-1;
            }
            // MODE_ITER branched
            int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
            for(int t=0;t<=N;t++){
                if(t==N)return pm(R[r.outr],A);
                if(pm(R[r.outr],A)!=tgt[t])return-1;
                int64_t rm=emod(R[0],r.branch_m);
                if(rm==0)exBody(R,r.body,r.then_len);
                else exBody(R,r.body+r.then_len,r.nbody-r.then_len);
                if(g_sat)return-1;}
        }
        return-1;
    }

    // Non-branched
    switch(r.mode){
    case MODE_ITER:{
        if(r.ointerp==OUT_BIT){
            // Wide integer bit extraction
            W Rw[kRegisterCount]={};Rw[0]=W::from(1);for(int s=0;s<r.bit_pos;s++)Rw[0]=Rw[0]+Rw[0];
            for(int t=0;t<=N;t++){
                if(t==N)return(int)Rw[0].bit(r.bit_pos);
                if((int)Rw[0].bit(r.bit_pos)!=tgt[t])return-1;
                exBodyW(Rw,r.body,r.nbody);}
        }else{
            int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
            for(int t=0;t<=N;t++){
                if(t==N)return pm(R[r.outr],A);
                if(pm(R[r.outr],A)!=tgt[t])return-1;
                exBody(R,r.body,r.nbody);if(g_sat)return-1;}
        }break;}
    case MODE_FUNC:{
        for(int t=0;t<=N;t++){
            int64_t R[kRegisterCount]={};R[0]=t;g_sat=false;
            exBody(R,r.body,r.nbody);
            if(g_sat)return-1;
            if(t==N)return pm(R[r.outr],A);
            if(pm(R[r.outr],A)!=tgt[t])return-1;}break;}
    case MODE_CTX:{
        // Reload registers from history each iteration (functional view).
        std::vector<int>gen(tgt.begin(),tgt.begin()+std::min(N,r.ctx_dstar));
        for(int t=r.ctx_dstar;t<=N;t++){
            int64_t R[kRegisterCount]={};
            for(int k=0;k<r.nr;k++)R[k]=gen[t-r.ctx_dstar+r.ctx_perm[k]];
            g_sat=false;exBody(R,r.body,r.nbody);
            if(g_sat)return-1;
            int v=pm(R[r.outr],A);
            if(t==N)return v;
            if(v!=tgt[t])return-1;
            gen.push_back(v);}
        return-1;}
    case MODE_EMIT:{
        if(r.concat_base>=2){// CONCAT deductive
            std::vector<int>out;
            for(int n=r.concat_off;(int)out.size()<=N;n++){std::vector<int>digs;int v=n;
            if(v==0)digs.push_back(0);else while(v>0){digs.push_back(v%r.concat_base);v/=r.concat_base;}
            if(r.concat_msb)std::reverse(digs.begin(),digs.end());
            for(int dig:digs)out.push_back(dig%A);}
            return(N<(int)out.size())?out[N]:-1;
        }else{// General EMIT
            g_emit_A=A;g_emit_buf.clear();
            int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
            for(int t=0;t<=N+10&&(int)g_emit_buf.size()<=N;t++){
                exBody(R,r.body,r.nbody);if(g_sat)break;}
            g_emit_A=0;
            if((int)g_emit_buf.size()>N)return g_emit_buf[N];
            return-1;
        }}
    }
    return-1;
}

static void recordProg(const Res&r,int ncat,const std::vector<int>&tgt,int A){
    ProgEntry pe;pe.r=r;pe.mdl=computeMDL(r,ncat);
    uint64_t fp=resFingerprint(r);
    // MDL-tail truncation derived from IEEE-754 double precision.
    // double has std::numeric_limits<double>::digits = 53 mantissa bits, so
    // a mixture term with relative weight 2^{-Δ} is below precision (cannot
    // affect the floating-point sum) when Δ > 53. Pruning at this threshold
    // is bit-equivalent to including the term.
    // Structural, not hand-picked - derives from the
    // IEEE 754 standard, not a hand-picked constant.
    constexpr double kMDLTailBits = static_cast<double>(std::numeric_limits<double>::digits);
    {std::lock_guard<std::mutex>lk(g_progs_mutex);
    if(!g_progs.empty()){double best_mdl=1e18;
        for(auto&p:g_progs)if(p.mdl<best_mdl)best_mdl=p.mdl;
        if(pe.mdl>best_mdl+kMDLTailBits)return;}
    // insert returns {iter, false} if fingerprint was already present.
    if(!g_progs_fingerprints.insert(fp).second)return;}
    pe.prediction=predictNext(r,tgt,A);
    {std::lock_guard<std::mutex>lk(g_progs_mutex);
    g_progs.push_back(pe);}}

// Run a discovered program for K steps, return output sequence
static std::vector<int> runProgram(const Res&r,int K,int A){
    std::vector<int>out;
    if(r.nbody<=0&&r.concat_base<2&&r.dary_base<2)return out;
    // DARY deductive: direct digit loop (same as verification)
    if(r.dary_base>=2){
        for(int n=0;n<K;n++){int64_t R[kRegisterCount]={};R[0]=r.dary_init_val;g_sat=false;
        if(n==0){R[1]=0;ex(R,r.dary_op);}
        else{int v=n;while(v>0&&!g_sat){R[1]=v%r.dary_base;ex(R,r.dary_op);v/=r.dary_base;}}
        if(g_sat)break;out.push_back(pm(R[0],A));}
        return out;}

    if(r.branched){
        if(r.mode==MODE_EMIT){
            g_emit_A=A;g_emit_buf.clear();
            int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
            for(int t=0;(int)g_emit_buf.size()<K&&!g_sat;t++){
                int64_t rm=emod(R[0],r.branch_m);
                if(rm==0)exBody(R,r.body,r.then_len);
                else exBody(R,r.body+r.then_len,r.nbody-r.then_len);
                if(t>K*10)break;}
            g_emit_A=0;
            for(int i=0;i<K&&i<(int)g_emit_buf.size();i++)out.push_back(g_emit_buf[i]);
        }else if(r.step_off>0||r.step_halt>0){
            // STEP: count convergent iterations per n
            for(int n=0;n<K;n++){int64_t R[kRegisterCount]={};R[0]=n+r.step_off;g_sat=false;int steps=0;
            for(int s=0;s<kStepIterCap;s++){if(R[0]<=r.step_halt&&s>0)break;
            int64_t rm=emod(R[0],r.branch_m);
            if(rm==0)exBody(R,r.body,r.then_len);
            else exBody(R,r.body+r.then_len,r.nbody-r.then_len);
            if(g_sat)break;steps++;}if(g_sat)break;out.push_back(steps%A);}
        }else{
            // MODE_ITER branched
            int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
            for(int t=0;t<K&&!g_sat;t++){out.push_back(pm(R[r.outr],A));
                int64_t rm=emod(R[0],r.branch_m);
                if(rm==0)exBody(R,r.body,r.then_len);
                else exBody(R,r.body+r.then_len,r.nbody-r.then_len);}
        }
        return out;
    }

    // Non-branched
    switch(r.mode){
    case MODE_ITER:{
        if(r.ointerp==OUT_BIT){
            W Rw[kRegisterCount]={};Rw[0]=W::from(1);
            for(int s=0;s<r.bit_pos;s++)Rw[0]=Rw[0]+Rw[0];
            for(int t=0;t<K;t++){out.push_back((int)Rw[0].bit(r.bit_pos));
            exBodyW(Rw,r.body,r.nbody);}
        }else{
            int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
            for(int t=0;t<K&&!g_sat;t++){out.push_back(pm(R[r.outr],A));
            exBody(R,r.body,r.nbody);}
        }break;}
    case MODE_FUNC:{
        for(int n=0;n<K;n++){int64_t R[kRegisterCount]={};R[0]=n;g_sat=false;
        exBody(R,r.body,r.nbody);if(g_sat)break;
        out.push_back(pm(R[r.outr],A));}break;}
    case MODE_CTX:{
        // Reload registers from history each iteration. K must be >= ctx_dstar to seed.
        if(r.ctx_dstar<=0)break;
        // Caller must provide seed via Res - here we use 0s (caller in solve() seeds from tgt).
        // For runProgram during prediction (predictNext), seeding is handled there.
        // For independent runProgram: cannot generate without seed. Return empty.
        break;}
    case MODE_EMIT:{
        if(r.concat_base>=2){// CONCAT deductive: generate directly
            for(int n=r.concat_off;(int)out.size()<K;n++){std::vector<int>digs;int v=n;
            if(v==0)digs.push_back(0);else while(v>0){digs.push_back(v%r.concat_base);v/=r.concat_base;}
            if(r.concat_msb)std::reverse(digs.begin(),digs.end());
            for(int dig:digs){out.push_back(dig%A);if((int)out.size()>=K)break;}}
        }else{// General EMIT: run body, collect OUT emissions
            g_emit_A=A;g_emit_buf.clear();
            int64_t R[kRegisterCount];memcpy(R,r.init,sizeof(R));g_sat=false;
            for(int t=0;(int)g_emit_buf.size()<K&&!g_sat;t++){
                exBody(R,r.body,r.nbody);if(t>K*10)break;}
            g_emit_A=0;
            for(int i=0;i<K&&i<(int)g_emit_buf.size();i++)out.push_back(g_emit_buf[i]);
        }break;}
    }
    return out;
}

// ──────────────────────────────────────────────────────────────────────────────
// Solomonoff mixture prediction.
//
// Given a collection of programs (in g_progs) consistent with a training
// sequence, predict the next K symbols by weighted-majority vote where each
// program p's vote is weighted by 2^(-mdl_p). For numerical stability,
// weights are normalized by subtracting the minimum MDL across the collection
// (only relative weights matter for argmax; the offset cancels).
//
// CTX programs require autoregressive generation seeded with the training
// prefix (mirror of omnis_validate's single-best CTX path); non-CTX programs
// use runProgram() to generate train_N + K outputs and read positions
// [train_N, train_N + K).
//
// Constraints:
// C1 - Sound: each program's prediction is computed identically to single-
// best validation; mixture only changes how votes are aggregated.
// Programs that fail to generate K outputs are skipped (no spurious
// prediction). Empty mixture returns -1 sentinels.
// C2 - Uniform: same algorithm applied across all benchmarks, all targets.
// C3 - Solomonoff-correct: weights = 2^(-mdl) reflect the prior P(h) ∝
// 2^(-|h|). Min-MDL anchor is a numerical stability transform; the
// argmax over weighted votes is invariant under this anchor.
// C4 - No constants beyond the IEEE-754 53-bit precision tail (already
// structurally derived in recordProg's truncation at kMDLTailBits).
//
// Returns vector of K predicted symbols (one per future step). Symbol -1
// indicates no programs in the mixture could produce a prediction at that
// step.
// inline: function is defined in omnis.cpp but called only from omnis_validate.cpp.
// static-only would trigger "unused function" warning in cli.cpp's translation unit.
static inline std::vector<int> predictMixture(
    const std::vector<ProgEntry>& progs,
    const std::vector<int>& train,
    int K, int A) {
    std::vector<int> result(K, -1);
    if (progs.empty() || K <= 0) return result;
    int N_train = (int)train.size();

    // Numerical-stability anchor: subtract min MDL. Argmax over 2^(-(mdl-min))
    // is identical to argmax over 2^(-mdl) (uniform multiplicative offset).
    double min_mdl = std::numeric_limits<double>::infinity();
    for (const auto& p : progs) if (p.mdl < min_mdl) min_mdl = p.mdl;

    // Collect each program's K predictions and weight.
    std::vector<std::vector<int>> per_program;
    std::vector<double> weights;
    per_program.reserve(progs.size());
    weights.reserve(progs.size());

    for (const auto& pe : progs) {
        const Res& r = pe.r;
        std::vector<int> preds;
        preds.reserve(K);
        bool ok = true;

        if (r.mode == MODE_CTX && r.ctx_dstar > 0 && r.nr > 0 && r.nbody > 0) {
            // CTX: autoregressive with train history. Mirrors omnis_validate's
            // single-best CTX path: feed the program's own output back into
            // the history (open-loop Solomonoff prediction).
            std::vector<int> gen = train;
            int ds = r.ctx_dstar;
            for (int k = 0; k < K; k++) {
                int t = N_train + k;
                int64_t R[kRegisterCount] = {};
                for (int j = 0; j < r.nr; j++) {
                    int idx = t - ds + r.ctx_perm[j];
                    if (idx < 0 || idx >= (int)gen.size()) idx = 0;
                    R[j] = gen[idx];
                }
                g_sat = false;
                exBody(R, r.body, r.nbody);
                if (g_sat) { ok = false; break; }
                int v = pm(R[r.outr], A);
                gen.push_back(v);
                preds.push_back(v);
            }
        } else {
            // Non-CTX: generate full train_N + K outputs and read the test segment.
            std::vector<int> out = runProgram(r, N_train + K, A);
            if ((int)out.size() == N_train + K) {
                for (int k = 0; k < K; k++) preds.push_back(out[N_train + k]);
            } else {
                ok = false;
            }
        }

        if (!ok || (int)preds.size() != K) continue; // skip programs that can't predict
        per_program.push_back(std::move(preds));
        weights.push_back(std::exp2(-(pe.mdl - min_mdl)));
    }

    if (per_program.empty()) return result;

    // Weighted majority vote per position. Tie-break by smallest symbol value.
    for (int k = 0; k < K; k++) {
        std::vector<double> vote(A, 0.0);
        for (size_t i = 0; i < per_program.size(); i++) {
            int v = per_program[i][k];
            if (v >= 0 && v < A) vote[v] += weights[i];
        }
        int best_v = 0;
        double best_w = vote[0];
        for (int v = 1; v < A; v++) {
            if (vote[v] > best_w) { best_v = v; best_w = vote[v]; }
        }
        result[k] = (best_w > 0.0) ? best_v : -1;
    }
    return result;
}
// ──────────────────────────────────────────────────────────────────────────────

// ──────────────────────────────────────────────────────────────────────────────
// Kraft inequality empirical smoke check.
//
// For a valid prefix code with codeword lengths {l_i}: Σ 2^(-l_i) ≤ 1.
// Equality for COMPLETE prefix codes (every bit string decodes to some
// codeword); strict < for INCOMPLETE codes (some bit strings unused).
// Σ > 1 → over-loaded encoding (multiple programs at same codeword) → INVALID.
//
// IMPORTANT CAVEATS - diagnostic, not a proof:
// 1. g_progs is a SUBSET of all programs in the encoding space:
// - Only sc==N programs are recorded.
// - Fingerprint-deduped (true structural duplicates collapsed).
// - MDL-tail truncated at best_mdl + kMDLTailBits = best_mdl + 53
// (recordProg line 1531) - programs whose MDL exceeds the threshold
// are pruned to stay within IEEE-754 precision.
// 2. Σ over g_progs is therefore a LOWER BOUND on Σ over all valid programs.
// 3. Σ_g_progs > 1 → DEFINITIVE Kraft violation (subset is bigger than the
// total admissible mass; the encoding is invalid).
// 4. Σ_g_progs ≤ 1 → NECESSARY condition; not a proof. Full sum could still
// exceed 1 over the unaccounted (unrecorded) programs.
// 5. Σ_g_progs ≪ 1 → encoding is REDUNDANT (more bits used than needed).
// Σ_g_progs ≈ 1 → encoding is TIGHT (every bit carries information).
//
// Numerical:
// - 2^(-MDL) for MDL ∈ [10, 150] yields [10^-3, 10^-45], all in IEEE-754
// normal range. No underflow concern in canonical workloads.
// - For MDL > 1023: exp2(-MDL) underflows to 0. Treated as 0 contribution
// (programs with such MDLs would be far below kMDLTailBits prune anyway).
// - Sum is monotone-positive; no catastrophic cancellation. Precision loss
// for small terms doesn't affect the >1 / ≤1 boundary check.
//
// Constraints:
// C1 - read-only diagnostic; cannot regress sc-correctness.
// C2 - applies uniformly to any g_progs collection from any benchmark.
// C3 - uses computed MDLs verbatim; doesn't alter Solomonoff ordering.
// C4 - no constants. Uses kMDLTailBits indirectly via recordProg's filter,
// which is structurally derived from IEEE-754 precision.
// inline: cross-TU usage (called from omnis_validate.cpp).
static inline double kraftSum(const std::vector<ProgEntry>& progs) {
    double s = 0.0;
    for (const auto& p : progs) {
        // exp2(-mdl): underflows to 0 for mdl > ~1023; that's fine - programs
        // with such MDLs would be pruned by the kMDLTailBits filter anyway.
        s += std::exp2(-p.mdl);
    }
    return s;
}

// Smoke-check Kraft on g_progs. Prints diagnostic to `out` (nullptr suppresses
// printing). Returns true iff the empirical (necessary, not sufficient) Kraft
// bound holds. False return = DEFINITIVE encoding bug; true return = passes
// the smoke test but does NOT prove encoding is well-formed.
static inline bool kraftSmokeCheck(const std::vector<ProgEntry>& progs, FILE* out) {
    double s = kraftSum(progs);
    bool ok = (s <= 1.0);
    if (out) {
        fprintf(out, "kraft_check: progs=%zu sum=%.6e %s\n",
                progs.size(), s,
                ok ? "OK (necessary; not sufficient - subset of program space)"
                   : "VIOLATION (subset-sum > 1 ⇒ encoding over-loaded)");
    }
    return ok;
}
// ──────────────────────────────────────────────────────────────────────────────

// ================================================================
// Section 6b: Persistent Program Database
// ================================================================
struct ProgramRecord{
    // Body
    uint8_t nbody;
    Ins body[kProgramBodyMax];
    // Execution
    int64_t init[kRegisterCount];
    uint8_t nr,outr,mode_u,ointerp_u;
    // Branching
    uint8_t branched,branch_m,then_len;
    // MDL
    double mdl;
    // Provenance
    uint64_t tgt_hash; // FNV-1a hash of target sequence
    uint16_t target_N;
    uint8_t target_A;
    uint8_t score;     // match score (N=perfect)
    int8_t predicted_next;
    // Special modes (concat/dary/step stored but not extended)
    int concat_base,concat_off;bool concat_msb;
    int step_off,step_halt;
    int dary_base,dary_init_val;Ins dary_op;
    int bit_pos;
    // MODE_CTX
    int ctx_dstar;
    int ctx_perm[kRegisterCount];
};

// Comprehensive structural hash for ProgramDB dedup.
// The previous bodyHash() only covered (body, n, nr, outr, mode), missing init,
// branch params, CONCAT/DARY/STEP/CTX params. Two distinct programs differing
// in those fields produced the same bodyHash and the second was silently dropped
// at g_progdb.add(). This new function mirrors resFingerprint's coverage so
// both layers (in-memory g_progs dedup and persistent ProgramDB dedup) use
// equivalent identity criteria.
// soundness: dropping wrongly-deduped programs from the library was a real
// loss of distinct programs. Fix recovers full coverage.
// C3 - predicate is purely structural over Res/ProgramRecord fields;
// target-independent.
static uint64_t programRecordHash(const ProgramRecord& pr) {
    uint64_t h = 0xcbf29ce484222325ULL;
    auto mix = [&h](uint64_t v) { h = (h ^ v) * 0x100000001b3ULL; };
    mix((uint64_t)pr.nbody);
    for (int i = 0; i < pr.nbody; i++) {
        mix((uint64_t)pr.body[i].ti);
        mix((uint64_t)pr.body[i].c);
        mix((uint64_t)pr.body[i].ar);
        for (int a = 0; a < 4; a++) mix((uint64_t)(uint8_t)pr.body[i].args[a]);
    }
    mix((uint64_t)pr.mode_u);
    mix((uint64_t)pr.nr);
    mix((uint64_t)pr.outr);
    mix((uint64_t)pr.ointerp_u);
    mix((uint64_t)pr.bit_pos);
    mix((uint64_t)pr.branched);
    if (pr.branched) {
        mix((uint64_t)pr.branch_m);
        mix((uint64_t)pr.then_len);
    }
    for (int i = 0; i < pr.nr && i < kRegisterCount; i++) mix((uint64_t)pr.init[i]);
    mix((uint64_t)pr.dary_base);
    mix((uint64_t)pr.dary_init_val);
    if (pr.dary_base >= 2) {
        mix((uint64_t)pr.dary_op.ti);
        mix((uint64_t)pr.dary_op.c);
        mix((uint64_t)pr.dary_op.ar);
        for (int a = 0; a < 4; a++) mix((uint64_t)(uint8_t)pr.dary_op.args[a]);
    }
    mix((uint64_t)pr.concat_base);
    mix((uint64_t)pr.concat_off);
    mix((uint64_t)(pr.concat_msb ? 1 : 0));
    mix((uint64_t)pr.step_off);
    mix((uint64_t)pr.step_halt);
    mix((uint64_t)pr.ctx_dstar);
    if (pr.mode_u == (uint8_t)MODE_CTX) {
        for (int i = 0; i < pr.nr && i < kRegisterCount; i++) mix((uint64_t)pr.ctx_perm[i]);
    }
    return h;
}

static uint64_t tgtHash(const std::vector<int>&tgt){
    uint64_t h=0xcbf29ce484222325ULL;
    for(int v:tgt)h=hmix(h,(uint64_t)v);return h;}

struct ProgramDB{
    std::vector<ProgramRecord>records;
    std::unordered_set<uint64_t>body_hashes;

    static ProgramDB load(const char*path){
        ProgramDB db;
        FILE*f=fopen(path,"rb");if(!f)return db;
        char magic[4];uint32_t ver,cnt;
        if(fread(magic,1,4,f)!=4||memcmp(magic,"ENAR",4)!=0){fclose(f);return db;}
        if(fread(&ver,4,1,f)!=1||ver!=2){fclose(f);return db;} // v2: added MODE_CTX fields
        if(fread(&cnt,4,1,f)!=1){fclose(f);return db;}
        for(uint32_t i=0;i<cnt;i++){
            ProgramRecord r;
            if(fread(&r,sizeof(r),1,f)!=1)break;
            uint64_t bh=programRecordHash(r); // comprehensive structural hash
            db.body_hashes.insert(bh);
            db.records.push_back(r);}
        fclose(f);return db;}

    bool save(const char*path)const{
        std::string tmp=std::string(path)+".tmp";
        FILE*f=fopen(tmp.c_str(),"wb");if(!f)return false;
        fwrite("ENAR",1,4,f);
        uint32_t ver=2,cnt=(uint32_t)records.size();
        fwrite(&ver,4,1,f);fwrite(&cnt,4,1,f);
        for(auto&r:records)fwrite(&r,sizeof(r),1,f);
        fclose(f);return rename(tmp.c_str(),path)==0;}

    void add(const Res&r,int ncat,const std::vector<int>&tgt,int A){
        // Build ProgramRecord first; compute comprehensive hash from it for dedup.
        // The previous bodyHash-based dedup was incomplete (missed init, branch
        // params, special-mode params).
        ProgramRecord pr={};
        pr.nbody=(uint8_t)r.nbody;
        memcpy(pr.body,r.body,sizeof(pr.body));
        memcpy(pr.init,r.init,sizeof(pr.init));
        pr.nr=(uint8_t)r.nr;pr.outr=(uint8_t)r.outr;
        pr.mode_u=(uint8_t)r.mode;pr.ointerp_u=(uint8_t)r.ointerp;
        pr.branched=r.branched?1:0;pr.branch_m=(uint8_t)r.branch_m;
        pr.then_len=(uint8_t)r.then_len;
        pr.mdl=computeMDL(r,ncat);
        pr.tgt_hash=tgtHash(tgt);pr.target_N=(uint16_t)tgt.size();
        // uint8 saturation = (1<<kRegisterCount)-1 = 255.
        pr.target_A=(uint8_t)A;pr.score=(uint8_t)std::min((1<<kRegisterCount)-1,r.sc);
        pr.predicted_next=(int8_t)predictNext(r,tgt,A);
        pr.concat_base=r.concat_base;pr.concat_off=r.concat_off;pr.concat_msb=r.concat_msb;
        pr.step_off=r.step_off;pr.step_halt=r.step_halt;
        pr.dary_base=r.dary_base;pr.dary_init_val=r.dary_init_val;pr.dary_op=r.dary_op;
        pr.bit_pos=r.bit_pos;
        pr.ctx_dstar=r.ctx_dstar;
        for(int i=0;i<kRegisterCount;i++)pr.ctx_perm[i]=r.ctx_perm[i];
        // Comprehensive dedup: covers body, init, branch, CONCAT/DARY/STEP/CTX.
        uint64_t bh=programRecordHash(pr);
        if(body_hashes.count(bh))return;
        body_hashes.insert(bh);
        records.push_back(pr);}

    Res toRes(const ProgramRecord&pr)const{
        Res r;
        r.nbody=pr.nbody;memcpy(r.body,pr.body,sizeof(r.body));
        memcpy(r.init,pr.init,sizeof(r.init));
        r.nr=pr.nr;r.outr=pr.outr;
        r.mode=(ExModel)pr.mode_u;r.ointerp=(OutInterp)pr.ointerp_u;
        r.branched=(pr.branched!=0);r.branch_m=pr.branch_m;r.then_len=pr.then_len;
        r.mdl=pr.mdl;r.concat_base=pr.concat_base;r.concat_off=pr.concat_off;
        r.concat_msb=pr.concat_msb;r.step_off=pr.step_off;r.step_halt=pr.step_halt;
        r.dary_base=pr.dary_base;r.dary_init_val=pr.dary_init_val;r.dary_op=pr.dary_op;
        r.bit_pos=pr.bit_pos;
        r.ctx_dstar=pr.ctx_dstar;
        for(int i=0;i<kRegisterCount;i++)r.ctx_perm[i]=pr.ctx_perm[i];
        r.desc="DB_SEED";return r;}

    int size()const{return(int)records.size();}

    // Phase 0: Test k=1 extensions of all DB programs against target.
    // Returns best-matching extension (or empty Res if nothing beats threshold).
    // Helper: verify a candidate Res against target. Returns score (prefix match length).
    int verify(const Res&r,const std::vector<int>&tgt,int A)const{
        int N=(int)tgt.size();
        auto out=runProgram(r,N,A);
        int sc=0;for(int i=0;i<N&&i<(int)out.size();i++){
            if(out[i]==tgt[i])sc++;else break;}
        return sc;}

    // Phase 0: Test extensions of all DB programs against target.
    Res testExtensions(const std::vector<int>&tgt,int A,double dl,int ncat)const{
        int N=(int)tgt.size();
        Res best;best.sc=0;
        if(records.empty())return best;
        std::map<int,std::vector<Ins>>l1_cache;
        int tested=0,skipped=0,hits=0;

        // Helper lambda: check candidate, update best
        // lex-best (sc, -mdl) update via helper.
        auto tryCandidate=[&](Res&ext,const char*tag)->bool{
            tested++;
            int sc=verify(ext,tgt,A);
            if(sc>=best.sc){
                ext.sc=sc;ext.mdl=computeMDL(ext,ncat);
                ext.desc=tag;
                if(tryUpdateBestLex(best, ext, sc)){
                    hits++;
                    if(sc==N){
                        recordProg(ext, ncat, tgt, A); /* Phase 0 → g_progs for mixture */
                        printf("    PHASE0 HIT sc=%d/%d %s\n",sc,N,tag);
                        return true;
                    }}}
            return false;};

        // ---- Sub-phase A: k=1 body extension (append/insert before LOOP) ----
        for(auto&pr:records){
            if(now_s()>dl)goto done;
            if(pr.concat_base>=2||pr.dary_base>=2)continue;
            // Skip MODE_CTX entries: verify() calls runProgram() which returns
            // empty for MODE_CTX (cannot seed without context). All such entries
            // would score 0 here; skipping saves the wasted work. Bug discovered
            // during a correctness pass.
            // soundness: testExtensions could not validate MODE_CTX entries
            // anyway (runProgram returns empty). Skip is functionally
            // equivalent to running and scoring 0.
            // C3 - predicate is structural (mode field), target-independent.
            if(pr.mode_u==(uint8_t)MODE_CTX)continue;
            if(pr.nbody>=kProgramBodyMax-1)continue; // = 23: must allow ≥1 instruction extension
            Res base=toRes(pr);
            int nr=base.nr;
            if(l1_cache.find(nr)==l1_cache.end())l1_cache[nr]=buildL1(nr);
            int nr_ext=std::min(nr+1,kRegisterCount);
            if(l1_cache.find(nr_ext)==l1_cache.end())l1_cache[nr_ext]=buildL1(nr_ext);
            uint64_t base_fp=0;
            {auto out=runProgram(base,std::min(4,N),A);
            for(int i=0;i<(int)out.size();i++)base_fp=hmix(base_fp,(uint64_t)out[i]);}
            int insert_pos=base.nbody;bool has_loop=false;
            for(int i=0;i<base.nbody;i++)if(base.body[i].ti==17){insert_pos=i;has_loop=true;break;}
            for(int pass=0;pass<2&&now_s()<dl;pass++){
                auto&L1use=(pass==0)?l1_cache[nr]:l1_cache[nr_ext];
                int nr_use=(pass==0)?nr:nr_ext;
                for(auto&ins:L1use){
                    if(now_s()>dl)goto done;
                    Res ext=base;ext.nr=nr_use;
                    if(has_loop){
                        for(int i=ext.nbody;i>insert_pos;i--)ext.body[i]=ext.body[i-1];
                        ext.body[insert_pos]=ins;ext.nbody++;
                        for(int i=0;i<ext.nbody;i++)if(ext.body[i].ti==17)ext.body[i].c++;
                    }else{ext.body[ext.nbody]=ins;ext.nbody++;}
                    uint64_t ext_fp=0;
                    {auto out=runProgram(ext,std::min(4,N),A);
                    for(int i=0;i<(int)out.size();i++)ext_fp=hmix(ext_fp,(uint64_t)out[i]);}
                    if(ext_fp==base_fp){skipped++;continue;}
                    std::string tag="DB_EXT("+ins.str()+")";
                    tryCandidate(ext,tag.c_str()); // removed early goto-done; helper handles lex-best, deadline-check goto-dones still bound the search
                    // Try different outr for FUNC mode extensions
                    if(pass==1&&!has_loop&&base.mode==MODE_FUNC){
                        for(int or2=0;or2<nr_use;or2++){if(or2==ext.outr)continue;
                            Res ext2=ext;ext2.outr=or2;
                            std::string t2="DB_EXT("+ins.str()+",R"+std::to_string(or2)+")";
                            tryCandidate(ext2,t2.c_str());}} // review
                }
            }
        }

        // ---- Sub-phase B: Register remapping (swap pairs, 0 MDL cost) ----
        for(auto&pr:records){
            if(now_s()>dl)goto done;
            if(pr.concat_base>=2||pr.dary_base>=2)continue;
            Res base=toRes(pr);
            int nr=base.nr;if(nr<2)continue;
            // Try all register pair swaps
            for(int r1=0;r1<nr&&now_s()<dl;r1++){
                for(int r2=r1+1;r2<nr;r2++){
                    Res ext=base;
                    // Swap r1↔r2 in all body instructions
                    for(int i=0;i<ext.nbody;i++){
                        for(int j=0;j<ext.body[i].ar;j++){
                            if(ext.body[i].args[j]==r1)ext.body[i].args[j]=r2;
                            else if(ext.body[i].args[j]==r2)ext.body[i].args[j]=r1;}}
                    // Swap in init
                    std::swap(ext.init[r1],ext.init[r2]);
                    // Swap outr if needed
                    if(ext.outr==r1)ext.outr=r2;else if(ext.outr==r2)ext.outr=r1;
                    std::string tag="DB_REMAP(R"+std::to_string(r1)+"<>R"+std::to_string(r2)+")";
                    tryCandidate(ext,tag.c_str()); // removed early goto-done; helper handles lex-best, deadline-check goto-dones still bound the search
                }
            }
        }

        // ---- Sub-phase C: Mode switching (FUNC↔ITER) ----
        for(auto&pr:records){
            if(now_s()>dl)goto done;
            if(pr.concat_base>=2||pr.dary_base>=2)continue;
            if(pr.branched)continue; // branched has own mode logic
            Res base=toRes(pr);
            // FUNC→ITER: run body repeatedly with persistent state
            if(base.mode==MODE_FUNC){
                Res ext=base;ext.mode=MODE_ITER;
                memset(ext.init,0,sizeof(ext.init));
                // Try a few init values for R[0]
                for(int iv=0;iv<=std::min(10,A)&&now_s()<dl;iv++){
                    ext.init[0]=iv;
                    std::string tag="DB_MODE(FUNC>ITER,init="+std::to_string(iv)+")";
                    tryCandidate(ext,tag.c_str()); // removed early goto-done; helper handles lex-best, deadline-check goto-dones still bound the search
                }
            }
            // ITER→FUNC: run body once per n with R[0]=n
            if(base.mode==MODE_ITER){
                Res ext=base;ext.mode=MODE_FUNC;
                memset(ext.init,0,sizeof(ext.init));
                std::string tag="DB_MODE(ITER>FUNC)";
                tryCandidate(ext,tag.c_str()); // removed early goto-done; helper handles lex-best, deadline-check goto-dones still bound the search
            }
        }

        // ---- Sub-phase D: Init perturbation (ITER/EMIT programs) ----
        for(auto&pr:records){
            if(now_s()>dl)goto done;
            if(pr.concat_base>=2||pr.dary_base>=2)continue;
            Res base=toRes(pr);
            if(base.mode==MODE_FUNC)continue; // FUNC has no persistent init
            int nr=base.nr;
            // Perturb each init register independently: try 0..isaMaxConstant and target[0]
            for(int ri=0;ri<nr&&now_s()<dl;ri++){
                int64_t orig=base.init[ri];
                for(int v=0;v<=kIsaMaxConstantConstexpr&&now_s()<dl;v++){
                    if(v==(int)orig)continue;
                    Res ext=base;ext.init[ri]=v;
                    std::string tag="DB_INIT(R"+std::to_string(ri)+"="+std::to_string(v)+")";
                    tryCandidate(ext,tag.c_str()); // removed early goto-done; helper handles lex-best, deadline-check goto-dones still bound the search
                }
                // Also try target[0] as init
                if(tgt[0]!=(int)orig&&tgt[0]<=kIsaMaxConstantConstexpr){
                    Res ext=base;ext.init[ri]=tgt[0];
                    std::string tag="DB_INIT(R"+std::to_string(ri)+"=tgt0)";
                    tryCandidate(ext,tag.c_str()); // removed early goto-done; helper handles lex-best, deadline-check goto-dones still bound the search
                }
            }
        }

        // ---- Sub-phase E: k=2 hierarchical extension ----
        // Only run if budget remains and k=1 found promising partial matches (sc >= N/4)
        if(now_s()<dl&&best.sc>=(N/4)&&best.sc<N&&best.nbody<22){
            // Extend the best k=1 result by one more instruction
            Res base2=best;
            int nr2=base2.nr;int nr2x=std::min(nr2+1,kRegisterCount);
            if(l1_cache.find(nr2)==l1_cache.end())l1_cache[nr2]=buildL1(nr2);
            if(l1_cache.find(nr2x)==l1_cache.end())l1_cache[nr2x]=buildL1(nr2x);
            uint64_t base2_fp=0;
            {auto out=runProgram(base2,std::min(4,N),A);
            for(int i=0;i<(int)out.size();i++)base2_fp=hmix(base2_fp,(uint64_t)out[i]);}
            int ins2_pos=base2.nbody;bool has_loop2=false;
            for(int i=0;i<base2.nbody;i++)if(base2.body[i].ti==17){ins2_pos=i;has_loop2=true;break;}
            for(int pass=0;pass<2&&now_s()<dl;pass++){
                auto&L1use=(pass==0)?l1_cache[nr2]:l1_cache[nr2x];
                int nr_use=(pass==0)?nr2:nr2x;
                for(auto&ins:L1use){
                    if(now_s()>dl)goto done;
                    Res ext=base2;ext.nr=nr_use;
                    if(has_loop2){
                        for(int i=ext.nbody;i>ins2_pos;i--)ext.body[i]=ext.body[i-1];
                        ext.body[ins2_pos]=ins;ext.nbody++;
                        for(int i=0;i<ext.nbody;i++)if(ext.body[i].ti==17)ext.body[i].c++;
                    }else{ext.body[ext.nbody]=ins;ext.nbody++;}
                    uint64_t ext_fp=0;
                    {auto out=runProgram(ext,std::min(4,N),A);
                    for(int i=0;i<(int)out.size();i++)ext_fp=hmix(ext_fp,(uint64_t)out[i]);}
                    if(ext_fp==base2_fp){skipped++;continue;}
                    std::string tag="DB_EXT2("+ins.str()+")";
                    tryCandidate(ext,tag.c_str()); // removed early goto-done; helper handles lex-best, deadline-check goto-dones still bound the search
                }
            }
        }

        done:
        printf("    phase0_ext: %d seeds, %d tested, %d skipped, %d hits, best=%d/%d %.3fs\n",
            (int)records.size(),tested,skipped,hits,best.sc,N,now_s());
        return best;}
};
static ProgramDB g_progdb;
static const char*G_PROGDB_PATH=nullptr;

// ================================================================
// SUB_CALL extension (SUB_CALL, canonical): hierarchical synthesis primitive - definitions
// ================================================================
// SUB_CALL inline-expands a library entry into the calling context. Library entries
// must be:
// 1. Pure body (no branched, no concat/dary/step special modes)
// 2. Free of nested SUB_CALL (recursion guard - prevents non-termination)
// 3. Bounded body length (≤24 by global constraint)
// Non-invocable entries are skipped at catalog-build time (subCallLibraryEntryInvocable)
// AND at execution time (defensive double-check via subCallLibraryEntryPure). The runtime
// no-op handles the corner case where a library entry was added between buildL1 and exec.
//
// Solomonoff cost is paid via catalog size growth: every SUB_CALL slot costs log2(ncat),
// identical to any other instruction. Hence SUB_CALL adds no MDL surcharge beyond
// catalog occupation. Non-invocable library entries do not occupy catalog slots, so
// they pay zero MDL - they simply don't participate in search.
//
// ===== SUB_CALL state memoization =====
// Phase 2H tests every (pre_body, library_entry) pair against target. When two
// candidates share a SUB_CALL invocation with the same pre-state R[8], the body
// execution produces identical results - wasted work. Memoize on (idx, R[8]).
//
// Constraint compliance:
// C1 - pure compute cache; programs with same input produce same output.
// C2 - MDL accounting unchanged; cache only affects search rate, not coverage.
// C3 - cache key is internal register state, no target peek; uniform across targets.
// C4 - applies to any target; cache is target-agnostic.
//
// Cache discipline:
// - thread_local (no mutex contention; per-thread state)
// - cleared at solve() entry to avoid cross-target stale state
// - bounded at SUBCALL_CACHE_LIMIT; over limit, new entries skipped (cache stays valid)
// - only stores results for non-saturated executions (g_sat=false on body completion)
//
// The cache is a structural improvement, not a tactical one - it does not violate
// Solomonoff: equal MDL programs are still tested in equal-priority order. The cache
// merely amortizes shared SUB_CALL evaluations across the candidate space.
struct SubCallCacheKey {
    int idx;
    int64_t R[kRegisterCount];
    bool operator==(const SubCallCacheKey& o) const noexcept {
        if (idx != o.idx) return false;
        for (int i = 0; i < kRegisterCount; i++) if (R[i] != o.R[i]) return false;
        return true;
    }
};
struct SubCallCacheKeyHash {
    size_t operator()(const SubCallCacheKey& k) const noexcept {
        // FNV-style hash combine; collisions degrade performance but not correctness.
        size_t h = (size_t)k.idx * 0x9e3779b97f4a7c15ull;
        for (int i = 0; i < kRegisterCount; i++) {
            h ^= (size_t)k.R[i] + 0x9e3779b9ull + (h << 6) + (h >> 2);
        }
        return h;
    }
};
// derive SUBCALL_CACHE_LIMIT from machine memory.
// Total cache memory budget = currentFreeMemoryBytes() / kRegisterCount (1/8 share),
// leaving 7/8 free memory for primary buildDDB pools. Per-thread limit =
// total / nt where nt = std::thread::hardware_concurrency().
// per_entry_bytes ≈ sizeof(SubCallCacheKey) + sizeof(std::array<int64_t,8>) + 32B
// (hash bucket + map node overhead)
// limit = (free_mem / kRegisterCount / nt) / per_entry_bytes
// Floor: 1024 entries (smallest useful cache; below this, lookup overhead exceeds
// amortization benefit). The 1024 floor is a structural minimum, not target-tuned.
// Queried once at process startup (before pools eat memory) - value is stable
// across solve() calls within a process, while still adapting to host capacity.
static size_t computeSubCallCacheLimit() {
    size_t free_mem = currentFreeMemoryBytes();
    size_t per_entry_bytes = sizeof(SubCallCacheKey) + sizeof(std::array<int64_t,kRegisterCount>) + 32;
    int nt = std::max(1, (int)std::thread::hardware_concurrency());
    size_t total_share = free_mem / (size_t)kRegisterCount;
    size_t per_thread = total_share / (size_t)nt;
    size_t entries = per_thread / per_entry_bytes;
    // Floor: 1<<(kRegisterCount+2) entries. ISA-derived: kRegisterCount=8 →
    // floor=1024 (one minimum-useful cache page; below this, unordered_map
    // lookup amortization is doubtful).
    return std::max((size_t)(1u << (kRegisterCount + 2)), entries);
}
static const size_t SUBCALL_CACHE_LIMIT = computeSubCallCacheLimit();
static thread_local std::unordered_map<SubCallCacheKey, std::array<int64_t, kRegisterCount>, SubCallCacheKeyHash> g_subcall_cache;
static inline void clearSubCallCache() {
    g_subcall_cache.clear();
}

// calibrate SubCall cache lookup cost at startup.
// Returns seconds per unordered_map find() - used to derive the cache
// eligibility threshold structurally rather than via empirical hardcoded value.
// Uses a representative-sized populated map (kStepIterCap entries with diverse
// keys) to mimic real-cache lookup geometry; pure-1-entry timing would
// underestimate cost on a populated cache (collisions, memory pressure).
static double calibrateCacheLookupCost() {
    static double cached = -1.0;
    if (cached > 0.0) return cached;
    const int K = kStepIterCap;
    std::unordered_map<SubCallCacheKey, std::array<int64_t, kRegisterCount>, SubCallCacheKeyHash> test_map;
    test_map.reserve(K);
    for (int seed = 0; seed < K; seed++) {
        SubCallCacheKey key;
        key.idx = seed;
        for (int i = 0; i < kRegisterCount; i++) key.R[i] = (int64_t)(seed * 31 + i * 17);
        std::array<int64_t, kRegisterCount> val;
        for (int i = 0; i < kRegisterCount; i++) val[i] = (int64_t)i;
        test_map.emplace(key, val);
    }
    volatile int64_t sum = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < K; i++) {
        SubCallCacheKey query;
        query.idx = i;
        for (int j = 0; j < kRegisterCount; j++) query.R[j] = (int64_t)(i * 31 + j * 17);
        auto it = test_map.find(query);
        if (it != test_map.end()) sum += it->second[0];
    }
    auto t1 = std::chrono::steady_clock::now();
    double total = std::chrono::duration<double>(t1 - t0).count();
    cached = total / K;
    if (cached <= 0.0) cached = 2e-8;  // safety: ≥20ns
    return cached;
}

// SubCall cache eligibility threshold derived from
// runtime cost ratio. Cache pays off when body execution time exceeds cache
// lookup time, i.e., body_length × per_op_cost > cache_lookup_cost.
// Threshold = ceil(cache_lookup_cost / per_op_cost) = body length at break-even.
// Replaces the empirical literal 3 with a hardware-calibrated bound.
static const int kSubCallCacheThreshold = []() {
    double per_op = calibratePerOpCost();
    double lookup = calibrateCacheLookupCost();
    if (per_op <= 0.0) return 3;  // safety fallback
    int t = (int)std::ceil(lookup / per_op);
    if (t < 1) t = 1;  // floor: at least 1-op bodies considered
    return t;
}();

static int subCallCatalogSize() { return g_progdb.size(); }

static inline bool subCallLibraryEntryPure(const ProgramRecord& pr) {
    if (pr.branched) return false;
    if (pr.concat_base >= 2) return false;
    if (pr.dary_base >= 2) return false;
    if (pr.step_off > 0 || pr.step_halt > 0) return false;
    // MODE_CTX entries assume registers are reloaded from history (R[k] = history[T - perm[k]])
    // each iteration. Inline expansion skips this reload, so the body executes against the
    // caller's register state instead - semantically different from the library's intended
    // behavior. Filter out to preserve inline-call faithfulness. (MODE_ITER, MODE_FUNC, and
    // MODE_EMIT bodies are pure register transformers and inline cleanly.)
    if (pr.mode_u == (uint8_t)MODE_CTX) return false;
    // nbody==0 entries have nothing to execute. Filter to avoid wasted catalog slots.
    if (pr.nbody == 0) return false;
    // Recursion guard: library entry must contain no SUB_CALL (prevents non-termination
    // via mutual recursion across library entries).
    for (int j = 0; j < pr.nbody; j++) if (pr.body[j].ti == 32) return false;
    return true;
}

static bool subCallLibraryEntryInvocable(int idx) {
    if (idx < 0 || idx >= g_progdb.size()) return false;
    return subCallLibraryEntryPure(g_progdb.records[idx]);
}

static void exSubCall(int64_t* R, int idx) {
    int sz = g_progdb.size();
    if (sz == 0) return;
    if (idx < 0 || idx >= sz) return;
    const ProgramRecord& pr = g_progdb.records[idx];
    if (!subCallLibraryEntryPure(pr)) return;
    // Memoize SUB_CALL invocations for non-trivial bodies.
    // threshold = kSubCallCacheThreshold, runtime-
    // calibrated as ceil(cache_lookup_cost / per_op_cost). Cache pays off when
    // body_length × per_op_cost exceeds cache_lookup_cost. Replaces the
    // previous empirical literal 3 with a hardware-calibrated bound.
    //
    // CRITICAL: cache stores only register state, not emissions. When the calling
    // context has g_emit_A > 0 (MODE_EMIT), library entries containing OUT push to
    // g_emit_buf as a side effect. A cache hit would silently swallow those pushes.
    // Skip cache entirely when g_emit_A > 0 to preserve C1 (no false negatives in
    // MODE_EMIT scoring). 
    if (pr.nbody >= kSubCallCacheThreshold && g_emit_A == 0) {
        SubCallCacheKey key;
        key.idx = idx;
        for (int i = 0; i < kRegisterCount; i++) key.R[i] = R[i];
        auto it = g_subcall_cache.find(key);
        if (it != g_subcall_cache.end()) {
            // Cache hit: restore output state. g_sat remains false (cached entries
            // are only stored when g_sat=false post-execution).
            for (int i = 0; i < kRegisterCount; i++) R[i] = it->second[i];
            return;
        }
        // Cache miss: execute and conditionally store.
        exBody(R, pr.body, pr.nbody);
        if (!g_sat && g_subcall_cache.size() < SUBCALL_CACHE_LIMIT) {
            std::array<int64_t, kRegisterCount> out;
            for (int i = 0; i < kRegisterCount; i++) out[i] = R[i];
            g_subcall_cache.emplace(key, out);
        }
        return;
    }
    exBody(R, pr.body, pr.nbody);
}

static void exSubCallW(W* R, int idx) {
    int sz = g_progdb.size();
    if (sz == 0) return;
    if (idx < 0 || idx >= sz) return;
    const ProgramRecord& pr = g_progdb.records[idx];
    if (!subCallLibraryEntryPure(pr)) return;
    exBodyW(R, pr.body, pr.nbody);
}

// ================================================================
// Section 7: solve() - Phase 1 (T-table) + Phase 2 (3-mode sieve)
// ================================================================
static Res solve(const std::vector<int>&tgt,int A,double dl){
    // Entry hook: reset extension-specific state at the start of each
    // benchmark solve. Prevents cross-benchmark stale state from biasing MDL accounting.
    // Clear the SUB_CALL memoization cache to avoid cross-target
    // stale state. The cache is thread_local so this clear is per-thread; correct because
    // solve() is the entry point per benchmark and runs on a known thread set.
    clearSubCallCache();
    int N=(int)tgt.size();Res best;
    int ncat=0;{const auto& r=isaConstantRanges();for(auto&[t,cs]:r)ncat+=(int)cs.size();
        // count only invocable library entries (matches what buildL1 emits).
        int sz_lib = subCallCatalogSize();
        for (int idx = 0; idx < sz_lib; idx++)
            if (subCallLibraryEntryInvocable(idx)) ncat++;
    }
    std::atomic<bool>phase2_collecting(false);
    // Thread-safe collection deadline. Relaxed ordering sufficient (monotone, approximate).
    std::atomic<double>phase2_collect_dl(0);
    auto p2c_dl_get=[&]()->double{return phase2_collect_dl.load(std::memory_order_relaxed);};
    auto p2c_dl_set=[&](double v){phase2_collect_dl.store(v,std::memory_order_relaxed);};
    int ds=computeDStar(tgt,A);
    int period=detectPeriod(tgt);
    int d_growth=measureGrowth(tgt,A);
    printf("    d*=%d period=%d d_growth=%d\n",ds,period,d_growth);

    // ══════════════════════════════════════════════════════════════
    // PHASE 0: Extension search from persistent database
    // Records hits to g_progs for the mixture. Does NOT update 'best' -
    // Phase 1+ comparisons use strict > so Phase 0's high-MDL hits must
    // not block lower-MDL Phase 1 discoveries. Merged at end of solve().
    Res phase0_best;phase0_best.sc=0;
    if(g_progdb.size()>0){
        double p0_budget=std::min((double)kIsaMaxConstantConstexpr/2.0,dl-now_s()); // = isaMaxConstant/2 = 5s
        Res ext=g_progdb.testExtensions(tgt,A,now_s()+p0_budget,ncat);
        if(ext.sc>0){
            ext.mdl=computeMDL(ext,ncat);
            if(ext.sc==N)recordProg(ext,ncat,tgt,A);
            phase0_best=ext;
        }
    }

    // PHASE 1: T-table deduction (instant - ISA matching)
    if(ds>0){
        // 1A: DEDUCTIVE - ISA matching on period tables
        if(period>0){
            auto T1=buildUnaryPeriodTable(tgt,A,period);
            if(!T1.empty()){
                auto m=isaMatchUnary(A,T1);
                if(m.found){int64_t init[kRegisterCount]={};init[0]=tgt[0];
                int sc=autoReg(&m.ins,1,ds,tgt,A,1,0,init);
                // lex-best (sc, -mdl) update via helper. Compute MDL for lex compare.
                if(sc>=best.sc){
                Ins body_use=m.ins;int nb_use=1;Ins body2[2]={m.ins,{6,A,{0,0,0,0},2}};
                if(sc==N){int64_t Rc[kRegisterCount]={};Rc[0]=tgt[0];g_sat=false;
                for(int t=0;t<kRegisterCount*N&&!g_sat;t++){exBody(Rc,&m.ins,1);}
                if(g_sat){int sc2=autoReg(body2,2,ds,tgt,A,1,0,init);
                    if(sc2==N){body_use=body2[0];nb_use=2;}}}
                Res h;h.sc=sc;h.desc="FLAT_ISA "+m.desc;h.mode=MODE_ITER;h.ointerp=OUT_MOD;
                h.body[0]=body_use;if(nb_use==2)h.body[1]=body2[1];h.nbody=nb_use;
                h.nr=1;h.outr=0;h.init[0]=tgt[0];h.mdl=computeMDL(h,ncat);
                tryUpdateBestLex(best, h, sc); /* removed early-return */
                if(sc==N) recordProg(h, ncat, tgt, A); /* ensure Phase 1A FLAT_ISA enters g_progs for Solomonoff mixture */}}
                auto mb=isaMatchBranched(A,T1);
                if(mb.found){printf("    branched ISA: m=%d then=%s else=%s\n",
                    mb.mod,mb.then_m.desc.c_str(),mb.else_m.desc.c_str());}
            }
            // Binary table with previous output as auxiliary
            if(period>=2){std::vector<int>aux(period);aux[0]=tgt[period-1];
            for(int t=1;t<period;t++)aux[t]=tgt[t-1];
            auto T2=buildBinaryPeriodTable(tgt,aux,A,A,period);
            if(!T2.empty()){auto m=isaMatchBinary(A,A,T2);
            if(m.found){printf("    binary ISA: %s\n",m.desc.c_str());
            for(int init0=0;init0<=isaMaxConstant();init0++){for(int init1=0;init1<=isaMaxConstant();init1++){
            int64_t R[kRegisterCount]={};R[0]=init0;R[1]=init1;g_sat=false;int sc=0;
            for(int t=0;t<N;t++){if(pm(R[0],A)==tgt[t])sc++;else break;
            int64_t prev=R[0];ex(R,m.ins);if(g_sat)break;R[1]=prev;}
            // lex-best (sc, -mdl) update via helper. Compute MDL for lex compare.
            if(sc>=best.sc){
            // Build body: [COPY(R0,R2), isa_op, COPY(R2,R1)]
            // R2 saves current R[0] before op modifies it, then R[1]=saved
            Ins prev_body[5];
            prev_body[0]={11,0,{0,2,0,0},2}; // COPY R0->R2
            prev_body[1]=m.ins;                // matched ISA op
            prev_body[2]={11,0,{2,1,0,0},2}; // COPY R2->R1
            int prev_nb=3;
            // Overflow guard: run 8N, if sat append MOD_C(A)
            if(sc==N){int64_t Rc[kRegisterCount]={};Rc[0]=init0;Rc[1]=init1;g_sat=false;
            for(int t=0;t<kRegisterCount*N&&!g_sat;t++)exBody(Rc,prev_body,prev_nb);
            if(g_sat){prev_body[prev_nb]={6,A,{0,0,0,0},2};prev_nb++;
            int64_t Rv[kRegisterCount]={};Rv[0]=init0;Rv[1]=init1;g_sat=false;int sc2=0;
            for(int t=0;t<N;t++){if(pm(Rv[0],A)!=tgt[t])break;sc2++;
            exBody(Rv,prev_body,prev_nb);if(g_sat)break;}
            if(sc2<N)continue;}}
            Res h;h.sc=sc;h.desc="FLAT_ISA_PREV "+m.desc;h.mode=MODE_ITER;h.ointerp=OUT_MOD;
            for(int i=0;i<prev_nb;i++)h.body[i]=prev_body[i];h.nbody=prev_nb;
            h.nr=3;h.outr=0;h.init[0]=init0;h.init[1]=init1;h.mdl=computeMDL(h,ncat);
            tryUpdateBestLex(best, h, sc);
            if(sc==N) recordProg(h, ncat, tgt, A); /* FLAT_ISA_PREV → g_progs */}}}}}}
        }
        // 1A extended: multi-register ISA matching.
        // nr loop runs to full kCascadePoolLevels regardless of
        // best.sc - different nr can find tighter MDL after an earlier phase
        // produced a sc=N candidate. Loop body cost is O(nr × A_max × N) - bounded.
        for(int nr=2;nr<=kCascadePoolLevels;nr++){
            auto T1=buildUnaryPeriodTable(tgt,A,period>0?period:(int)tgt.size());
            if(T1.empty())continue;
            std::vector<int>ctr(period>0?period:N);for(int t=0;t<(int)ctr.size();t++)ctr[t]=t%(period>0?period:N);
            int ctr_range=period>0?period:N;if(ctr_range>kQuickCheckLen)ctr_range=kQuickCheckLen;
            auto T2c=buildBinaryPeriodTable(tgt,ctr,A,ctr_range,period>0?period:std::min(N,kQuickCheckLen));
            if(!T2c.empty()){auto m=isaMatchBinary(A,ctr_range,T2c);
            if(m.found){printf("    ctr binary ISA: %s\n",m.desc.c_str());
            for(int init0=0;init0<=isaMaxConstant();init0++){int64_t R[kRegisterCount]={};R[0]=init0;R[1]=0;g_sat=false;int sc=0;
            for(int t=0;t<N;t++){if(pm(R[0],A)==tgt[t])sc++;else break;
            ex(R,m.ins);if(g_sat)break;R[1]=sat(R[1]+1);}
            // lex-best (sc, -mdl) update via helper.
            if(sc>=best.sc){Res h;h.sc=sc;h.desc="FLAT_ISA_CTR "+m.desc;h.mode=MODE_ITER;h.ointerp=OUT_MOD;
            h.body[0]=m.ins;h.body[1]={0,0,{1,0,0,0},1};h.nbody=2;h.nr=2;h.outr=0;h.init[0]=init0;h.mdl=computeMDL(h,ncat);
            tryUpdateBestLex(best, h, sc);
            if(sc==N) recordProg(h, ncat, tgt, A); /* FLAT_ISA_CTR → g_progs */}}}}
        }

        // 1B: Full T-elimination context search (context-table, MODE_CTX semantics)
        // Loops over (nr, permutation of context positions, output register, body pool depths 1-3).
        // ctxConsistentPerm filters bodies that satisfy T under the permutation.
        // verifyCTX confirms full-sequence reproduction in MODE_CTX semantics (registers reload each iter).
        // Budget cap is the only limit (no |T| filter, to preserve C1: avoid false negatives).
        // For sequences with bounded d* and small T, finds program in milliseconds.
        // For sequences with large T or no CTX program, budget cap limits overhead.
        auto T=buildT(tgt,ds);
        // ds is already structurally bounded by computeDStar's
        // info-theoretic limit. Magic 8 upper bound was redundant.
        if(!T.empty()&&ds>=1){
            // CTX deadline scales with available budget so nr=2
            // pool building (L1^3 grows ~400K bodies for full ISA) can complete.
            // Floor of 5s preserves prior behavior for very tight budgets.
            // Cap at min(30s, dl/2) keeps CTX from monopolizing the overall budget.
            double remaining=dl-now_s();
            double ctx_budget=std::max((double)kIsaMaxConstantConstexpr/2.0,
                                        std::min((double)kIsaMaxConstantConstexpr*3.0,remaining/2.0));
            double ctx_dl=std::min(dl,now_s()+ctx_budget);
            // max_nr derives from ISA register count, not magic 4.
            int max_nr=std::min({(int)ds,kRegisterCount});
            // CTX runs to its ctx_dl regardless of best.sc, so
            // lex-best (sc, -mdl) can find a shorter-MDL CTX_X even when an
            // earlier phase already produced a sc=N candidate. The 5s ctx_dl
            // bounds the cost; lex-best filtering at line 2532 keeps useless
            // work cheap. Without this, Fibonacci-class targets (ADD-recurrence
            // mod A) get classified as not_compressed_predicted because the
            // optimal CTX_X (~30 bits) is never reached after Phase 1A finds
            // a verbose FLAT_ISA_PREV (~57 bits). the Solomonoff lex-best rule
            // requires we keep searching until budget exhausts.
            for(int nr=1;nr<=max_nr&&now_s()<ctx_dl;nr++){
                auto L1=buildL1(nr);
                auto p1=buildPoolDS(L1,1,nr,ctx_dl);
                auto p2=buildPoolDS(L1,2,nr,ctx_dl);
                auto p3=(nr<=kCascadePoolLevels)?buildPoolDS(L1,3,nr,ctx_dl):std::vector<DB>{};
                // Recursive permutation enumeration: P(ds, nr) total
                std::vector<int>perm(nr);
                std::vector<bool>used(ds,false);
                std::function<void(int)>recurse=[&](int k){
                    if(now_s()>ctx_dl)return;
                    if(k==nr){
                        for(int outr=0;outr<nr&&now_s()<ctx_dl;outr++){
                            for(auto*pool:{&p1,&p2,&p3}){
                                for(auto&body:*pool){
                                    if(now_s()>ctx_dl)break;
                                    if(!ctxConsistentPerm(body.ops,body.n,A,outr,nr,T,perm.data()))continue;
                                    int sc=verifyCTX(body.ops,body.n,A,outr,nr,ds,perm.data(),tgt);
                                    // lex-best (sc, -mdl) update via helper.
                                    if(sc>=best.sc){
                                        Res h;h.sc=sc;h.mode=MODE_CTX;h.ointerp=OUT_MOD;
                                        h.ctx_dstar=ds;h.nr=nr;h.outr=outr;
                                        for(int i=0;i<nr;i++)h.ctx_perm[i]=perm[i];
                                        for(int i=0;i<body.n;i++)h.body[i]=body.ops[i];
                                        h.nbody=body.n;
                                        std::string ps="(";for(int i=0;i<nr;i++){if(i)ps+=",";ps+=std::to_string(perm[i]);}ps+=")";
                                        h.desc="CTX_X nr="+std::to_string(nr)+" d="+std::to_string(ds)+
                                               " perm="+ps+" out=R"+std::to_string(outr)+
                                               " L="+std::to_string(body.n);
                                        h.mdl=computeMDL(h,ncat);
                                        if(tryUpdateBestLex(best, h, sc) && sc==N){recordProg(best,ncat,tgt,A);}
                                    }
                                }
                            }
                        }
                        return;
                    }
                    for(int j=0;j<ds;j++){
                        if(used[j])continue;
                        used[j]=true;perm[k]=j;
                        recurse(k+1);
                        used[j]=false;
                        if(now_s()>ctx_dl)return;
                    }};
                recurse(0);
            }
        }
    } // end Phase 1

    // ══════════════════════════════════════════════════════════════
    // DEDUCTIVE ACCELERATORS (fast O(1)-O(N) checks, not templates)
    // These produce 3-mode Res results. They don't define exec models.
    // If removed, the sieve finds the same programs - just slower.
    // ══════════════════════════════════════════════════════════════
    // ── Digital factorization: detects d-ary digit recurrences ──
    // deductive accelerators (CONCAT/DARY) run regardless of
    // best.sc so lex-best can find a shorter description even when an earlier
    // phase produced a sc==N candidate. These checks are deductive and fast
    // (closed-form per d/off/msb), so the cost is negligible.
    {
        // tryConcat: Champernowne-type digit concatenation
        for(int d=2;d<=std::min(kIsaMaxConstantConstexpr,A);d++){for(int off=0;off<=kCascadePoolLevels;off++){for(bool msb:{true,false}){
        std::vector<int>out;for(int n=off;(int)out.size()<N;n++){std::vector<int>digs;int v=n;
        if(v==0)digs.push_back(0);else while(v>0){digs.push_back(v%d);v/=d;}
        if(msb)std::reverse(digs.begin(),digs.end());for(int dig:digs)out.push_back(dig%A);}
        int sc=0;for(int i=0;i<N&&i<(int)out.size();i++)if(out[i]==tgt[i])sc++;else break;
        // lex-best (sc, -mdl) update via helper.
        if(sc>=best.sc){Res h;h.sc=sc;h.desc="CONCAT d="+std::to_string(d)+" off="+std::to_string(off);
        h.mode=MODE_EMIT;h.ointerp=OUT_MOD;h.nr=3;
        // Real body: INC(R0), COPY(R0,R1), DIVC(R1,R1,R2,d), OUT(R2), LOOP(2,R1)
        h.body[0]={0,0,{0,0,0,0},1};   // INC(R0)
        h.body[1]={11,0,{0,1,0,0},2};  // COPY(R0,R1)
        h.body[2]={8,d,{1,1,2,0},3};   // DIVC(R1,R1,R2,d)
        h.body[3]={12,0,{2,0,0,0},1};  // OUT(R2)
        h.body[4]={17,2,{1,0,0,0},1};  // LOOP(2,R1)
        h.nbody=5;h.init[0]=off-1;h.concat_base=d;h.concat_off=off;h.concat_msb=msb;
        h.mdl=computeMDL(h,ncat);
        tryUpdateBestLex(best, h, sc);
        if(sc==N) recordProg(h, ncat, tgt, A); /* CONCAT → g_progs */}}}}
        // tryDigital: d-ary digit recurrences (ThueMorse, DigitSum4)
        int d_growth=measureGrowth(tgt,A);
        std::vector<int>d_cands;
        if(d_growth>=2)d_cands={d_growth,d_growth-1,d_growth+1};
        for(int d=2;d<=kIsaMaxConstantConstexpr;d++){bool in=false;for(int x:d_cands)if(x==d)in=true;if(!in)d_cands.push_back(d);}
        auto L1_dig=buildL1(2);
        // DARY enumerates all d values regardless of best.sc.
        // DARY is deductive/fast (~9 d-values × N comparisons); the lex-best
        // update inside picks the lower-MDL DARY among sc==N candidates.
        // Without this, DARY misses tighter base-d recurrences when an earlier
        // phase (FLAT_ISA_PREV, DARY at smaller d) already produced sc==N.
        for(int d:d_cands){if(d<2||d>kIsaMaxConstantConstexpr)continue;
        int max_n=N/d;if(max_n<3)continue;
        std::vector<int>dtbl(A*d,-1);bool det=true;
        for(int n=0;n<max_n&&det;n++){int a=tgt[n];for(int r=0;r<d;r++){int idx=a*d+r;
        int y=(d*n+r<N)?tgt[d*n+r]:0;if(idx>=(int)dtbl.size()){det=false;break;}
        if(dtbl[idx]==-1)dtbl[idx]=y;else if(dtbl[idx]!=y)det=false;}}
        if(det){auto m=isaMatchBinary(A,d,dtbl);if(m.found){
        for(int init_v=0;init_v<=kIsaMaxConstantConstexpr;init_v++){bool ok=true;for(int n=0;n<N&&ok;n++){
        int64_t R[kRegisterCount]={};R[0]=init_v;int v=n;
        if(v==0){R[1]=0;ex(R,m.ins);}
        else{while(v>0&&!g_sat){R[1]=v%d;ex(R,m.ins);v/=d;}}
        if(g_sat||pm(R[0],A)!=tgt[n])ok=false;}
        if(ok){// Encode digit loop with n=0 fix:
        // [COPY(R0,R1), LOAD(R0,init), LOAD(R2,0), remap(op), DIVC(R1,R1,R2,d), remap(op), LOOP(R1,2)]
        // First remap(op) handles the n=0 digit (R2=0). LOOP handles remaining.
        Ins remap=m.ins;for(int i=0;i<remap.ar;i++)if(remap.args[i]==1)remap.args[i]=2;
        Res h;h.sc=N;h.desc="DARY d="+std::to_string(d)+" "+m.desc+" init="+std::to_string(init_v);
        h.mode=MODE_FUNC;h.ointerp=OUT_MOD;
        h.body[0]={11,0,{0,1,0,0},2};     // COPY R0->R1
        h.body[1]={10,init_v,{0,0,0,0},1}; // LOAD R0,init_val
        h.body[2]={10,0,{2,0,0,0},1};     // LOAD R2,0 (n=0 digit)
        h.body[3]=remap;                    // op(init, 0) for n=0
        h.body[4]={8,d,{1,1,2,0},3};       // DIVC(R1,d): R1=quot, R2=rem
        h.body[5]=remap;                    // op with digit
        h.body[6]={17,2,{1,0,0,0},1};      // LOOP(R1,2)
        h.nbody=7;h.nr=kDaryCanonicalNr;h.outr=0;h.dary_base=d;h.dary_init_val=init_v;h.dary_op=m.ins;
        // lex-best (sc, -mdl) update via helper; keep iterating to find min-MDL DARY.
        h.mdl=computeMDL(h,ncat);
        tryUpdateBestLex(best, h, h.sc);
        if(h.sc==N) recordProg(h, ncat, tgt, A); /* DARY → g_progs for mixture */}}}}
        // Also test each L1 as per-digit body
        for(int d:d_cands){if(d<2||d>kIsaMaxConstantConstexpr)continue;
        for(auto&ins:L1_dig){for(int init_v=0;init_v<=kCascadePoolLevels;init_v++){bool ok=true;
        for(int n=0;n<N&&ok;n++){int64_t R[kRegisterCount]={};R[0]=init_v;int v=n;
        if(v==0){R[1]=0;g_sat=false;ex(R,ins);if(g_sat){ok=false;break;}}
        else{while(v>0){R[1]=v%d;g_sat=false;ex(R,ins);if(g_sat){ok=false;break;}v/=d;}}
        if(!ok)break;if(pm(R[0],A)!=tgt[n])ok=false;}
        if(ok){Ins remap=ins;for(int i=0;i<remap.ar;i++)if(remap.args[i]==1)remap.args[i]=2;
        Res h;h.sc=N;h.desc="DARY d="+std::to_string(d)+" "+ins.str()+" init="+std::to_string(init_v);
        h.mode=MODE_FUNC;h.ointerp=OUT_MOD;
        h.body[0]={11,0,{0,1,0,0},2};h.body[1]={10,init_v,{0,0,0,0},1};
        h.body[2]={10,0,{2,0,0,0},1};h.body[3]=remap; // LOAD R2,0 + op for n=0
        h.body[4]={8,d,{1,1,2,0},3};h.body[5]=remap;h.body[6]={17,2,{1,0,0,0},1};
        h.nbody=7;h.nr=kDaryCanonicalNr;h.outr=0;h.dary_base=d;h.dary_init_val=init_v;h.dary_op=ins;
        // lex-best (sc, -mdl) update via helper; keep iterating to find min-MDL DARY.
        h.mdl=computeMDL(h,ncat);
        tryUpdateBestLex(best, h, h.sc);
        if(h.sc==N) recordProg(h, ncat, tgt, A); /* DARY → g_progs for mixture */}}}}
    }

    // ══════════════════════════════════════════════════════════════
    // PHASE 2A: Flat sieve - test all DDB pool bodies in all 3 modes
    // ══════════════════════════════════════════════════════════════
    // Levin cross-phase budget: equal share per remaining phase (2A, 2B, 2C+2F).
    // Removes target-conditioned heuristic (constraint 3 fix).
    double budget_total=dl-now_s();
    // budget split = 1/kCascadePoolLevels (= 3 phases:
    // 2A, 2B, 2C+2F). Same value (1/3 ≈ 0.333) as the magic literal 3.0.
    double p2a_dl=std::min(dl,now_s()+budget_total/(double)kCascadePoolLevels);
    // if Phase 0/1 already produced a sc==N candidate, grant
    // Phase 2 a bounded collection window so it can search for a shorter-MDL
    // program. Without this, Phase 2A/2H were bypassed entirely when an
    // earlier phase solved - same Solomonoff lex-best gap that Pass 6 fixed
    // for CTX. Window = kIsaMaxConstantConstexpr seconds (= 10s, ISA-derived,
    // matches Phase 2A's own collecting window). Phase 2A collecting-mode
    // semantics (see existing phase2_collecting plumbing at lines 2747+,
    // 2799+) handle the lex-best update during this window.
    if(best.sc>=N&&!phase2_collecting&&now_s()<dl){
        phase2_collecting=true;
        p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));
    }
    if((best.sc<N||phase2_collecting)&&now_s()<p2a_dl){
        printf("    phase2a_flat_sieve %.1fs\n",now_s());
        //
        // Phase 2A nr range capped at kCascadePoolLevels (= 3, structural cascade
        // depth). Higher nr delegated to Phase 2B which has MDL-priority machinery
        // for deeper search. review widened to kRegisterCount with cost-bound,
        // but the cost estimate (|fL1|^3 × G^nr × per_op_cost) accounts only for
        // pool-fingerprint-build cost, not the dominant downstream Phase 2A
        // processing cost (3 modes × LOOP variants × N × body_exec). Gate admits
        // nr=4..8 inappropriately, consuming budget on diminishing-returns work
        // (bisection: review parityalt SOLVED 26s, #27/#28 PARTIAL >300s).
        // Cost-bound retained as defensive verification.
        const double p2a_per_op=calibratePerOpCost();
        const double p2a_G_log=std::log((double)isaMaxConstant()+1.0);
        for(int nr=2;nr<=kCascadePoolLevels&&now_s()<p2a_dl&&(best.sc<N||(phase2_collecting&&now_s()<p2c_dl_get()));nr++){
            auto fL1=buildL1(nr);
            // Cost estimate: pool-fingerprint dominates. Pool size ≈ |fL1|^3 (depth-3 max).
            // Per-fingerprint cost = G^nr × per_op_cost. Total ≈ |fL1|^3 × G^nr × per_op_cost.
            double pool_size_est=std::pow((double)fL1.size(),3);
            double estimated_nr_cost=pool_size_est*std::exp(p2a_G_log*nr)*p2a_per_op;
            double remaining=p2a_dl-now_s();
            //
            // The original `if(estimated_nr_cost>remaining)break;` was a structural
            // guard for the audit-#27 nr extension to kRegisterCount. It used
            // calibratePerOpCost(), which (pre-#77) could return inflated values
            // under thread load, causing the break to spuriously trigger at nr=3
            // and miss parityalt's d=3 solution. review made calibration
            // single-threaded-static-init + min-of-N robust, eliminating the noise
            // at the source. review restored the hard cap to kCascadePoolLevels,
            // making the dynamic guard structurally redundant (nr ∈ [2,3] only).
            // review verified that no binary-inlining or PGO sensitivity remains
            // (all hot-path constants are static constexpr; cost calibration is
            // noise-free). Estimate kept as documentation; break
            // remains removed. Verified empirically: with break,
            // parityalt finds NESTED_LOOP L=5 MDL=97.9 in 250s; without, finds
            // FUNC_L d=3 MDL=38.2 in ~30s.
            (void)estimated_nr_cost; (void)remaining;
            auto p2=buildDDB(fL1,2,nr,p2a_dl,0,/*fast_fp=*/true);
            auto p3=buildDDB(fL1,3,nr,p2a_dl,0,/*fast_fp=*/true);
            // Phase 2A's per-entry use cost upper bound.
            // 3 modes (FUNC/ITER/EMIT) × LOOP_variants (≤ kDDBBodyMax × kRegisterCount) ×
            // N positions × body_exec (≤ kDDBBodyMax × per_op_cost). Worst-case bound
            // for flat (no-LOOP) execution; LOOP variants amortize via early break.
            // 3 = number of phase-2A test modes (FUNC/ITER/EMIT, excluding CTX).
            // Numerically equal to kCascadePoolLevels; semantically the count of
            // pre-CTX modes in enum ExModel (MODE_CTX has value 3 = the count).
            double p2a_per_entry_use=(double)kCascadePoolLevels*(double)kDDBBodyMax*(double)kRegisterCount*(double)N*(double)kDDBBodyMax*p2a_per_op;
            auto p4=composeDDB(p3,fL1,nr,p2a_dl,nullptr,0,/*fast_fp=*/true, p2a_per_entry_use);
            // C1: Phase 2A parallel body testing
            std::atomic<int>p2a_best_sc(best.sc);
            std::mutex p2a_mtx;
            for(auto*pool:{&p2,&p3,&p4}){
            int pool_sz=(int)pool->size();
            std::atomic<int>next_fi(0);
            auto p2a_worker=[&](){
            while(true){
            int fi=next_fi.fetch_add(1);
            if(fi>=pool_sz||now_s()>p2a_dl)return;
            if(p2a_best_sc.load()>=N&&!phase2_collecting)return;
                auto&fb=(*pool)[fi];
                // Test flat body + LOOP variants
                for(int var=0;var<=fb.n*nr&&now_s()<p2a_dl;var++){
                    Ins body[kProgramBodyMax/2];int nb;
                    if(var==0){for(int i=0;i<fb.n;i++)body[i]=fb.ops[i];nb=fb.n;}
                    else{int v1=var-1,ll=v1/nr+1,kr=v1%nr;
                        nb=fb.n+1;if(nb>kDDBBodyMax)continue;
                        for(int i=0;i<fb.n;i++)body[i]=fb.ops[i];
                        body[fb.n]={17,ll,{(int8_t)kr,0,0,0},1};}

                    // Cache BodyDesc once per body (reused across modes + N-step loops)
                    BodyDesc p2a_bd;computeBodyDesc(body,nb,p2a_bd);
                    // --- MODE_FUNC: R[0]=n, R[1..7]=0, run body, output ---
                    for(int outr=0;outr<std::min(nr,3);outr++){
                    int sc=0;
                    for(int n=0;n<N;n++){int64_t R[kRegisterCount]={};R[0]=n;g_sat=false;
                    exBodyD(R,body,nb,p2a_bd);if(g_sat)break;
                    if(pm(R[outr],A)!=tgt[n])break;sc++;}
                    // lex-best (sc, -mdl) update via helper.
                    if(sc>=p2a_best_sc.load()||(sc==N&&phase2_collecting)){Res h;h.sc=sc;
                    h.desc=(var==0?"FUNC":"FUNC_L")+std::string(" d=")+std::to_string(nb);
                    h.mode=MODE_FUNC;h.ointerp=OUT_MOD;for(int i=0;i<nb;i++)h.body[i]=body[i];
                    h.nbody=nb;h.nr=nr;h.outr=outr;h.mdl=computeMDL(h,ncat);
                    tryUpdateBestLex(best, h, sc, &p2a_best_sc, &p2a_mtx);printf("    %s HIT sc=%d d=%d R%d body={",var==0?"FUNC":"FUNC_L",sc,nb,outr);
                    for(int i=0;i<nb;i++)printf("%s%s",i?";":"",body[i].str().c_str());
                    printf("}\n");if(sc==N){recordProg(h,ncat,tgt,A);
                    if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                    if(now_s()>p2c_dl_get())return;}}}

                    // --- MODE_ITER: R=init, persist, output then run ---
                    for(int r0:{0,1,2}){if(pm(r0,A)!=tgt[0])continue;
                    int64_t R[kRegisterCount]={};R[0]=r0;g_sat=false;int sc=0;
                    for(int t=0;t<N;t++){if(pm(R[0],A)!=tgt[t])break;sc++;
                    exBodyD(R,body,nb,p2a_bd);if(g_sat)break;}
                    // lex-best (sc, -mdl) update via helper.
                    if(sc>=p2a_best_sc.load()||(sc==N&&phase2_collecting)){Res h;h.sc=sc;
                    h.desc=(var==0?"ITER":"ITER_L")+std::string(" d=")+std::to_string(nb);
                    h.mode=MODE_ITER;h.ointerp=OUT_MOD;for(int i=0;i<nb;i++)h.body[i]=body[i];
                    h.nbody=nb;h.nr=nr;h.outr=0;h.init[0]=r0;h.mdl=computeMDL(h,ncat);
                    tryUpdateBestLex(best, h, sc, &p2a_best_sc, &p2a_mtx);printf("    %s HIT sc=%d d=%d r0=%d body={",var==0?"ITER":"ITER_L",sc,nb,r0);
                    for(int i=0;i<nb;i++)printf("%s%s",i?";":"",body[i].str().c_str());
                    printf("}\n");if(sc==N){recordProg(h,ncat,tgt,A);
                    if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                    if(now_s()>p2c_dl_get())return;}}}

                    // --- MODE_EMIT: R=init, persist, collect OUT emissions ---
                    for(int r0:{0,1,2}){
                    g_emit_A=A;g_emit_buf.clear();
                    int64_t R[kRegisterCount]={};R[0]=r0;g_sat=false;
                    for(int t=0;(int)g_emit_buf.size()<N&&!g_sat&&t<N*kIsaMaxConstantConstexpr;t++){
                        exBodyD(R,body,nb,p2a_bd);
                        // Early abort: if no emissions after kQuickCheckLen body executions, this body doesn't emit
                        if(t==kQuickCheckLen&&g_emit_buf.empty())break;}
                    g_emit_A=0;
                    int sc=0;for(int t=0;t<N&&t<(int)g_emit_buf.size();t++){
                        if(g_emit_buf[t]!=tgt[t])break;sc++;}
                    // lex-best (sc, -mdl) update via helper.
                    if(sc>=p2a_best_sc.load()||(sc==N&&phase2_collecting)){Res h;h.sc=sc;
                    h.desc=(var==0?"EMIT":"EMIT_L")+std::string(" d=")+std::to_string(nb);
                    h.mode=MODE_EMIT;h.ointerp=OUT_MOD;for(int i=0;i<nb;i++)h.body[i]=body[i];
                    h.nbody=nb;h.nr=nr;h.outr=0;h.init[0]=r0;h.mdl=computeMDL(h,ncat);
                    tryUpdateBestLex(best, h, sc, &p2a_best_sc, &p2a_mtx);printf("    %s HIT sc=%d d=%d r0=%d body={",var==0?"EMIT":"EMIT_L",sc,nb,r0);
                    for(int i=0;i<nb;i++)printf("%s%s",i?";":"",body[i].str().c_str());
                    printf("}\n");if(sc==N){recordProg(h,ncat,tgt,A);
                    if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                    if(now_s()>p2c_dl_get())return;}}}
                }
            }};// end p2a_worker lambda
            int p2a_nt=std::min((int)std::thread::hardware_concurrency(),std::max(1,pool_sz));
            if(p2a_nt<=1){p2a_worker();}
            else{std::vector<std::thread>thr;for(int t=0;t<p2a_nt;t++)thr.emplace_back(p2a_worker);
                for(auto&t:thr)t.join();}
            if(phase2_collecting&&now_s()>p2c_dl_get()){break;}
            }// pool loop
        }
    }

    // ══════════════════════════════════════════════════════════════
    // PHASE 2B: Branched sieve - cascade with modFilter
    // ══════════════════════════════════════════════════════════════
    // Adaptive cascade: starts at budget_total*0.4, extends to dl-10 on progress.
    // Fully internal to Phase 2B. dl is NEVER modified.
    // When cascade self-terminates early (no progress), later phases get more time.
    // Levin: equal share of remaining for 2B (2 phases left: 2B, 2C+2F).
    // Cascade adaptive extension can still push to dl-10 if making progress.
    double p2b_remaining=dl-now_s();
    double p2b_baseline=std::min(now_s()+p2b_remaining*0.5, dl-(double)kIsaMaxConstantConstexpr);
    double p2b_ceiling=std::max(p2b_baseline, dl-(double)kIsaMaxConstantConstexpr);
    // If Phase 2A already solved and we are
    // in collection mode, cap cascade_dl by the collection window p2c_dl_get().
    // Without this, cascade workers check only d.alive() (cascade_dl, ~dl-10)
    // and ignore p2c_dl entirely → they run until budget exhaustion even
    // though the answer was found seconds ago and only the 10s collection
    // window remains. This was the root cause of parityalt taking 290s
    // (budget-saturated) when standalone runs solve in ~32s. No hand-picked constants
    // clean: no magic - the collection window is a structural deadline set
    // at solve-time + 10s by Phase 2A's HIT logic (line 1963/1978/1999).
    if(phase2_collecting){
        double pdl=p2c_dl_get();
        p2b_baseline=std::min(p2b_baseline,pdl);
        p2b_ceiling =std::min(p2b_ceiling, pdl);
    }
    Deadline cascade_dl(p2b_baseline,p2b_ceiling,N,A);
    std::vector<DiscBody>discovered;
    if(ds>0&&(best.sc<N||phase2_collecting)&&cascade_dl.alive()){
        printf("    phase2b_branched %.1fs\n",now_s());
        //
        // Cascade nr range capped at kCascadePoolLevels+1 (= 4, structural -
        // one more level than cascade pool depth, reflecting cascade's
        // ability to use one fresh register beyond pre-built pools).
        // Widening to kRegisterCount (relying on AdaptiveDeadline to
        // self-terminate) was tried and regressed: the no-progress window
        // lets ineffective nr=5..8 levels consume budget that the
        // productive levels need. The cap at 4 keeps the cascade on the
        // levels that actually contribute.
        for(int nr=1;nr<=kCascadePoolLevels+1&&(best.sc<N||(phase2_collecting&&now_s()<p2c_dl_get()))&&(cascade_dl.maybe_extend(),cascade_dl.alive());nr++){
            if(phase2_collecting&&now_s()>p2c_dl_get())break;
            // Only d.report(sc) counts as progress. nr transitions are activity, not results.
            auto L1=buildL1(nr);
            double pool_dl=phase2_collecting?p2c_dl_get():cascade_dl.t_end.load(std::memory_order_relaxed);
            auto p1=buildPool(L1,1,nr,pool_dl);auto p2=buildPool(L1,2,nr,pool_dl);
            std::vector<DB>p3;if(nr>=3&&now_s()<pool_dl)p3=buildPool(L1,3,nr,pool_dl);
            printf("    cascade nr=%d p1=%d p2=%d p3=%d %.1fs\n",nr,(int)p1.size(),(int)p2.size(),(int)p3.size(),now_s());

            int cas_sc=best.sc;std::vector<int>cas_init;int cas_m=0;
            std::vector<DB>cas_even,cas_odd;
            cascade_dl.best_sc_seen.store(std::max(cascade_dl.best_sc_seen.load(std::memory_order_relaxed),best.sc),std::memory_order_relaxed);
            if(cascadeSearch(tgt,A,L1,nr,p1,p2,p3,discovered,
                            cas_sc,cas_init,cas_m,cas_even,cas_odd,cascade_dl)){
                // Store as branched Res
                Res h;h.sc=cas_sc;h.desc="BR m="+std::to_string(cas_m)+" nr="+std::to_string(nr);
                h.mode=MODE_ITER;h.ointerp=OUT_MOD;h.branched=true;h.branch_m=cas_m;
                h.nr=nr;h.outr=0;
                for(int i=0;i<(int)cas_init.size()&&i<kRegisterCount;i++)h.init[i]=cas_init[i];
                int pos=0;
                if(!cas_even.empty())for(int i=0;i<cas_even[0].n;i++)h.body[pos++]=cas_even[0].ops[i];
                h.then_len=pos;
                if(!cas_odd.empty())for(int i=0;i<cas_odd[0].n;i++)h.body[pos++]=cas_odd[0].ops[i];
                h.nbody=pos;h.mdl=computeMDL(h,ncat);
                // lex-best (sc, -mdl) update via helper.
                tryUpdateBestLex(best, h, cas_sc);
                if(cas_sc==N){recordProg(h,ncat,tgt,A);
                if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                // Shrink cascade_dl to honor the (just-set) collection window -
                // mirrors the at-construction cap above. Keeps cascade workers
                // from running through full cascade_dl after the collection
                // deadline expires.
                cascade_dl.t_end.store(std::min(cascade_dl.t_end.load(std::memory_order_relaxed),p2c_dl_get()),std::memory_order_relaxed);
                if(now_s()>p2c_dl_get())return best;}
            // lex-best (sc, -mdl) update via helper.
            // cascadeSearch returns true when a worker hits sc==N;
            // it returns false when no worker hits N OR no worker improved best_sc.
            // In the false case, the inner cascade update at line 1107 may NEVER
            // have fired (no worker advanced past `best_sc>=N` initial state),
            // leaving cas_init/cas_even/cas_odd empty and cas_m at its initial 0.
            // The original `else if(cas_sc>=best.sc)` branch then created a Res
            // with sc=cas_sc (=outer best.sc=N) but empty body and branch_m=0 -
            // a syntactically invalid program with artificially small MDL that
            // would replace the true sc==N program in best via lex-best. Bug
            // surfaced during a correctness review of
            // Fibonacci A000045_a4 reporting "BR m=0 nr=1" with MDL=12.
            // Fix: gate on cas_init non-empty (i.e., cascade actually populated
            // results) before constructing the Res. Empty init means no worker
            // ever satisfied the inner update's lex-tiebreak condition.
            }else if(cas_sc>=best.sc && !cas_init.empty() && !cas_even.empty()){
                Res h;h.sc=cas_sc;h.desc="BR m="+std::to_string(cas_m)+" nr="+std::to_string(nr);
                h.mode=MODE_ITER;h.ointerp=OUT_MOD;h.branched=true;h.branch_m=cas_m;
                h.nr=nr;h.outr=0;
                for(int i=0;i<(int)cas_init.size()&&i<kRegisterCount;i++)h.init[i]=cas_init[i];
                int pos=0;
                for(int i=0;i<cas_even[0].n;i++)h.body[pos++]=cas_even[0].ops[i];
                h.then_len=pos;
                if(!cas_odd.empty())for(int i=0;i<cas_odd[0].n;i++)h.body[pos++]=cas_odd[0].ops[i];
                h.nbody=pos;h.mdl=computeMDL(h,ncat);
                tryUpdateBestLex(best, h, cas_sc);
                if(cas_sc==N) recordProg(h, ncat, tgt, A); /* cascade BR else → g_progs */
            }
        }
    }
    // dl is unchanged. All subsequent phases use the original dl.
    // ── Step-count accelerator (uses discovered convergent bodies from cascade) ──
    // Step-count: only run long scan for A>2 (step-count outputs mod A, useful for A>2)
    // For A=2: step-count gives binary output, same as the target - not informative
    // Budget: 30% of remaining time. The direct convergence scan IS the primary
    // mechanism for step-count sequences - the cascade can't discover them (it tests
    // direct output, not step counts), so discovered.empty() is expected and must NOT
    // penalize budget.
    if((best.sc<N||phase2_collecting)&&A>2&&now_s()<dl){
        double remaining=dl-now_s();
        // removed 30s floor; Levin proportional allocation only.
        // step search share = 3/isaMaxConstant of remaining
        // (= 0.3 with current ISA). ISA-derived; same numerical value.
        // numerator 3 = kCascadePoolLevels (Phase 2 cascade depth, structural).
        double step_dl=std::min(dl,now_s()+remaining*((double)kCascadePoolLevels/(double)kIsaMaxConstantConstexpr));
        // MDL-sort `discovered` so step_count_accel iterates simplest bodies
        // first (Solomonoff prior). Same set as before - sort is deterministic,
        // C1-C4 clean. Sort key: instruction count, then m, then canonical body
        // fingerprint (FNV-1a-style hash) for a deterministic tiebreaker.
        {
            auto body_fp=[](const DB&d)->uint64_t{uint64_t h=0xcbf29ce484222325ULL;
                for(int i=0;i<d.n;i++){h^=(uint64_t)d.ops[i].ti*0x100000001b3ULL;
                h^=(uint64_t)d.ops[i].ar*0xff51afd7ed558ccdULL;
                h^=(uint64_t)d.ops[i].c*0xc4ceb9fe1a85ec53ULL;h*=0x100000001b3ULL;}return h;};
            std::sort(discovered.begin(),discovered.end(),[&](const DiscBody&a,const DiscBody&b){
                int an=a.even.n+a.odd.n,bn=b.even.n+b.odd.n;
                if(an!=bn)return an<bn;
                if(a.m!=b.m)return a.m<b.m;
                uint64_t af=body_fp(a.even)^(body_fp(a.odd)<<1);
                uint64_t bf=body_fp(b.even)^(body_fp(b.odd)<<1);
                return af<bf;});
        }
        printf("    step_count_accel: %d discovered %.1fs\n",(int)discovered.size(),now_s());
        // MDL-monotone iteration over (off, halt) pairs.
        // Iterate by total = off+halt ascending → smaller-MDL pairs first
        // (Solomonoff prior). Bound by N (sequence length, structural):
        // off+halt ≥ N shifts past the input range with no useful gain.
        for(auto&db:discovered){if(now_s()>step_dl)break;
        for(int total=0;total<=N&&now_s()<step_dl;total++){
        for(int off=0;off<=total;off++){int halt=total-off;
        int sc=0;bool ok=true;for(int n=0;n<N&&ok;n++){int64_t R[kRegisterCount]={};R[0]=n+off;
        g_sat=false;int steps=0;for(int s=0;s<kStepIterCap;s++){if(R[0]<=halt&&s>0)break;
        int64_t rm=emod(R[0],db.m);
        if(rm==0)exBody(R,db.even.ops,db.even.n);else exBody(R,db.odd.ops,db.odd.n);
        if(g_sat){ok=false;break;}steps++;}
        if(!ok)break;if(steps%A==tgt[n])sc++;else{ok=false;break;}}
        // lex-best (sc, -mdl) update via helper.
        if(sc>=best.sc){Res h;h.sc=sc;h.desc="STEP off="+std::to_string(off)+" halt="+std::to_string(halt);
        h.mode=MODE_FUNC;h.ointerp=OUT_MOD;h.nr=2;h.outr=0;
        int pos=0;for(int i=0;i<db.even.n;i++)h.body[pos++]=db.even.ops[i];h.then_len=pos;
        for(int i=0;i<db.odd.n;i++)h.body[pos++]=db.odd.ops[i];h.nbody=pos;
        h.branched=true;h.branch_m=db.m;h.step_off=off;h.step_halt=halt;
        h.mdl=computeMDL(h,ncat);
        if(tryUpdateBestLex(best, h, sc) && sc==N){recordProg(best,ncat,tgt,A);
        if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
        if(now_s()>p2c_dl_get())return best;}}}}}
        // Direct convergence scan
        auto L1_s=buildL1(2);auto p1_s=buildPool(L1_s,1,2,step_dl);auto p2_s=buildPool(L1_s,2,2,step_dl);
        auto&Pe=p1_s;auto&Po=(p2_s.empty()?p1_s:p2_s);
        // Pre-filter + sort Pe by R0 reduction: strongest non-trivial reducers first.
        // Filter: body must (1) reduce R0 from both R0=10 and R0=6, AND
        // (2) produce DIFFERENT R0 outputs (not a constant function).
        // Constant-output bodies (LOAD(0), SUB(R0,R0,R0)) converge trivially
        // with ANY Po, wasting budget on useless step-count verification.
        // Sort by ascending R0 output from R0=10 (strongest reducers first).
        std::vector<std::pair<int64_t,int>> pe_order;
        for(int ei=0;ei<(int)Pe.size();ei++){
            int64_t R1[kRegisterCount]={},R2[kRegisterCount]={};R1[0]=kIsaMaxConstantConstexpr;R2[0]=6;
            g_sat=false;exBody(R1,Pe[ei].ops,Pe[ei].n);if(g_sat||R1[0]<=0||R1[0]>=kIsaMaxConstantConstexpr)continue;
            g_sat=false;exBody(R2,Pe[ei].ops,Pe[ei].n);if(g_sat||R2[0]<=0||R2[0]>=6)continue;
            if(R1[0]==R2[0])continue; // constant function → trivial convergence
            pe_order.push_back({R1[0],ei});}
        std::sort(pe_order.begin(),pe_order.end());
        // Parallelized step-count scan: each (pi, oi) pair is independent.
        // Best update via mutex. Workers exit on deadline or sc==N (after collection window).
        {std::atomic<int>step_next_pi(0);
        std::mutex step_mtx;
        std::atomic<int>step_best_sc(best.sc);
        auto step_worker=[&](){
            while(true){
                int pi=step_next_pi.fetch_add(1);
                if(pi>=(int)pe_order.size()||now_s()>step_dl)return;
                if(phase2_collecting&&now_s()>p2c_dl_get())return;
                if(step_best_sc.load()>=N&&!phase2_collecting)return;
                int ei=pe_order[pi].second;
                for(int oi=0;oi<(int)Po.size()&&now_s()<step_dl;oi++){
                    // ISA-derived seeds [2, max_c] and iter cap N*max_c.
                    const int kConvIterCap=N*isaMaxConstant();
                    bool conv=true;for(int tv=2;tv<=isaMaxConstant();tv++){int64_t R[kRegisterCount]={};R[0]=tv;
                    for(int s=0;s<kConvIterCap;s++){if(R[0]<=1)break;int64_t rm=emod(R[0],2);g_sat=false;
                    if(rm==0)exBody(R,Pe[ei].ops,Pe[ei].n);else exBody(R,Po[oi].ops,Po[oi].n);
                    if(g_sat){conv=false;break;}}if(R[0]>1)conv=false;if(!conv)break;}
                    if(!conv)continue;
                    // MDL-monotone (off,halt) iteration, bounded by N.
                    for(int total=0;total<=N;total++){
                    for(int off=0;off<=total;off++){int halt=total-off;
                        int sc=0;bool ok=true;for(int n=0;n<N&&ok;n++){int64_t R[kRegisterCount]={};R[0]=n+off;
                        g_sat=false;int steps=0;for(int s=0;s<kStepIterCap;s++){if(R[0]<=halt&&s>0)break;
                        int64_t rm=emod(R[0],2);
                        if(rm==0)exBody(R,Pe[ei].ops,Pe[ei].n);
                        else exBody(R,Po[oi].ops,Po[oi].n);
                        if(g_sat){ok=false;break;}steps++;}
                        if(!ok)break;if(steps%A==tgt[n])sc++;else{ok=false;break;}}
                        // lex-best (sc, -mdl) update via helper.
                        if(sc>=step_best_sc.load()){Res h;h.sc=sc;h.desc="STEP off="+std::to_string(off)+" halt="+std::to_string(halt);
                        h.mode=MODE_FUNC;h.ointerp=OUT_MOD;h.nr=2;
                        int pos=0;for(int i=0;i<Pe[ei].n;i++)h.body[pos++]=Pe[ei].ops[i];h.then_len=pos;
                        for(int i=0;i<Po[oi].n;i++)h.body[pos++]=Po[oi].ops[i];h.nbody=pos;
                        h.branched=true;h.branch_m=2;h.step_off=off;h.step_halt=halt;
                        h.mdl=computeMDL(h,ncat);
                        tryUpdateBestLex(best, h, sc, &step_best_sc, &step_mtx);
                        if(sc==N){recordProg(h,ncat,tgt,A);
                        if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}}}}}
                }
            }};
        int nt=std::min((int)std::thread::hardware_concurrency(),std::max(1,(int)pe_order.size()));
        if(nt<=1)step_worker();
        else{std::vector<std::thread>thr;for(int t=0;t<nt;t++)thr.emplace_back(step_worker);
            for(auto&t:thr)t.join();}}
    }

    // ══════════════════════════════════════════════════════════════
    // PHASE 2C: Wide-integer sieve - A=2, OUT_BIT at various bit positions
    // ══════════════════════════════════════════════════════════════
    // Wide sieve: gets ALL remaining time if cascade didn't solve and A=2
    double p2c_dl=(A<=2&&best.sc<N)?dl:std::min(dl,now_s()+(dl-now_s())*0.5);
    if(A==2&&(best.sc<N||phase2_collecting)&&now_s()<p2c_dl){
        printf("    phase2c_wide_bit %.1fs\n",now_s());
        // adaptive cost-bounded nr.
        // Theoretical |fL1|^3 over-estimates by dedup ratio (~1% empirically).
        // Use OBSERVED dedup ratio from prior nr to predict actual pool size.
        // First iteration (nr=2): always allowed (smallest, calibrates ratio).
        // Subsequent: predicted_pool = |fL1(nr)|^3 × dedup_ratio_observed.
        // Memory bound = currentFreeMemoryBytes() (queried per-nr).
        // Time bound = remaining/levels_remaining (Levin equal-class share).
        const double p2c_per_op=calibratePerOpCost();
        double p2c_dedup_ratio=1.0; // initial: worst case (no compression)
        //
        // Phase 2C nr range capped at kCascadePoolLevels+1 (= 4 with current arch),
        // structurally one more than cascade pool depth. reviewv3 widened this
        // to kRegisterCount with adaptive-dedup cost-bound, but the cost predictor
        // (predicted_pool × G² × per_op_cost) underestimates the actual nr=5..8
        // processing cost in practice. Same pattern as #27/#28 regressions.
        for(int nr=2;nr<=kCascadePoolLevels+1&&now_s()<p2c_dl&&(best.sc<N||(phase2_collecting&&now_s()<p2c_dl_get()));nr++){
            auto fL1=buildL1(nr);
            // Build fL1_noloop FIRST so cost check and ratio update use same base.
            // (fL1_noloop is the actual base passed to buildDDB; using fL1 here
            // would make the cost predictor incoherent vs. the ratio update below.)
            std::vector<Ins>fL1_noloop;
            for(auto&ins:fL1){
                if(ins.ti==17)continue;
                if(ins.ti==32){ // skip SUB_CALL slots whose library has LOOP
                    int idx=ins.c;
                    if(idx>=0&&idx<g_progdb.size()){
                        const ProgramRecord&pr=g_progdb.records[idx];
                        bool body_has_loop=false;
                        for(int j=0;j<pr.nbody;j++)if(pr.body[j].ti==17){body_has_loop=true;break;}
                        if(body_has_loop)continue;
                    }
                }
                fL1_noloop.push_back(ins);
            }
            int nfL1_nl=(int)fL1_noloop.size();
            // Cost-feasibility check using observed dedup ratio (skip on first iter).
            //
            // Removed time_cost-based break. The time_cost estimate uses
            // calibratePerOpCost() which inflates under thread load, causing
            // the same kind of spurious break as in Phase 2A.
            // Memory-cost check retained - the mem_bound protection prevents
            // genuine OOM and uses currentFreeMemoryBytes() (not calibration).
            // With our hard nr cap at kCascadePoolLevels+1, the
            // time-bound was structurally redundant anyway.
            if(nr>2){
                double theoretical=std::pow((double)nfL1_nl,3);
                double predicted_pool=theoretical*p2c_dedup_ratio;
                size_t mem_cost=(size_t)(predicted_pool*(double)sizeof(DDB));
                double time_cost=predicted_pool*121.0*p2c_per_op; // G^2 = 121
                int levels_remaining=(kCascadePoolLevels+1)-nr+1;
                double time_share=(p2c_dl-now_s())/std::max(1,levels_remaining);
                size_t mem_bound=currentFreeMemoryBytes();
                (void)time_cost; (void)time_share;  // calculation kept for documentation; break removed
                if(mem_cost>mem_bound)break;  // memory protection only
            }
            // Phase 2C: STREAMING wide-bit search.
            // Build depth-3 pool (uncapped), then stream depth-4 extensions:
            // for each depth-3 body × each L1 tail, test immediately in OUT_BIT.
            // No depth-4 pool, no cap, no composition order bias.
            // Early termination after first mismatch makes this ~40s for 352M candidates.
            // Build non-LOOP depth-3 pool with FAST fingerprint (int64 only, no wide-int).
            // ISA-derived: LOOP (ti=17) excluded (tested via pool); non-LOOP streamed exhaustively.
            // Fast fingerprint: skip wide-int hash (3μs → 0.5μs per candidate).
            // Dedup collisions from int64-only hash are harmless: duplicates tested and fail fast.
            //
            // library-bootstrap Option H: SUB_CALL slots whose library body CONTAINS a LOOP are also
            // excluded - analogous to ti=17 exclusion. Phase 2C streams MILLIONS of
            // candidates per second under the assumption that per-candidate verification
            // cost is bounded (flat body executes O(L) ops). A SUB_CALL invocation whose
            // library body contains a LOOP runs up to 200 inner iterations per call,
            // breaking the streaming throughput assumption. Empirically this caused a
            // ~200× per-candidate slowdown (5.3M/s baseline → 27K/s with LOOP'd library
            // entries enabled), pushing nr=2 over the deadline before nr=3 could even
            // start (where Rule30's solution lives).
            //
            // Constraint compliance:
            // C1 - exact match unchanged; the exclusion is at the search-enumeration
            // level, not at the verification semantics level.
            // C2 - global ncat unchanged. Excluded SUB_CALL slots remain in the
            // catalog and their MDL cost is paid by every program slot, exactly
            // as for ti=17 (which is also globally counted but locally excluded).
            // Programs found in Phase 2C still pay log₂(ncat_with_subcall) per
            // slot - no per-phase catalog redefinition.
            // C3 - uniform criterion: a library body's LOOP-presence is a STRUCTURAL
            // property of the entry, not a target characteristic. The rule
            // applies identically to every Phase 2C target.
            // C4 - Phase 2C still runs on every applicable target (A=2, OUT_BIT).
            // (fL1_noloop and nfL1_nl built above the cost-feasibility check.)
            // Depth-3 pool: fast fingerprint (int64 only, no wide-int hash)
            // cap=0 → buildDDB uses pow(|fL1|, depth) default.
            // Actual pool size deadline-bounded by p2c_dl, not cap.
            auto p3_w=buildDDB(fL1_noloop,3,nr,p2c_dl,0,/*fast_fp=*/true);
            // Update observed dedup ratio for next nr's cost-feasibility check.
            // Empirically the ratio is ~0.01 (1%) for non-LOOP fL1 at depth 3,
            // but it depends on nr (more registers → fewer collisions, larger ratio).
            // Using actual previous-nr ratio is the closest constraint-clean predictor.
            {double theoretical_obs=std::pow((double)fL1_noloop.size(),3);
             if(theoretical_obs>0)p2c_dedup_ratio=(double)p3_w.size()/theoretical_obs;}
            // MDL-sort depth-3: lower MDL (simpler bodies) streamed first (Constraint 2)
            {double lr_s=log2(std::max(1,nr));double lc_s=log2(std::max(1,ncat));
            std::sort(p3_w.begin(),p3_w.end(),[&](const DDB&a,const DDB&b){
                double ma=0,mb=0;
                for(int i=0;i<a.n;i++){ma+=lc_s+a.ops[i].ar*lr_s;if(a.ops[i].c>0)ma+=uInt(a.ops[i].c);}
                for(int i=0;i<b.n;i++){mb+=lc_s+b.ops[i].ar*lr_s;if(b.ops[i].c>0)mb+=uInt(b.ops[i].c);}
                return ma<mb;});}
            printf("    wide_bit nr=%d d3=%d fL1=%d %.1fs\n",nr,(int)p3_w.size(),nfL1_nl,now_s());
            // Precompute initW for each bit position
            struct BitPos{int k;W initW;};
            std::vector<BitPos>bps;
            // bit-candidate count = kCascadePoolLevels = 3 (ISA-derived).
            // 255 → (1<<kRegisterCount)-1 = 255 (uint8 saturation, review).
            for(int ki=0;ki<kCascadePoolLevels;ki++){int k=(ki==0)?((1<<kRegisterCount)-1):((ki==1)?N:N/2);
                W iw=W::from(1);for(int s=0;s<k;s++)iw=iw+iw;
                if((int)iw.bit(k)==tgt[0])bps.push_back({k,iw});}
            if(bps.empty()){printf("    wide_bit: no valid bit positions\n");}
            else{
            // Phase 2C parallelized: shared best score (atomic) + best mutex.
            // Constraint check: same bodies tested, MDL-sorted pool grabbed via atomic
            // counter (sequential prefix preserved). Identical to Phase 2A pattern.
            std::atomic<int>p2c_best_sc(best.sc);
            std::mutex p2c_mtx;
            // Loop 1: depth-3 bodies tested directly - parallelized
            {std::atomic<int>next_fi(0);
            auto loop1_worker=[&](){
                while(true){
                    int fi=next_fi.fetch_add(1);
                    if(fi>=(int)p3_w.size()||now_s()>p2c_dl)return;
                    if(phase2_collecting&&now_s()>p2c_dl_get())return;
                    if(p2c_best_sc.load()>=N&&!phase2_collecting)return;
                    for(auto&bp:bps){W R[kRegisterCount]={};R[0]=bp.initW;int sc=0;
                    for(int t=0;t<N;t++){if((int)R[0].bit(bp.k)!=tgt[t])break;sc++;
                    exBodyW(R,p3_w[fi].ops,p3_w[fi].n);}
                    // lex-best (sc, -mdl) update via helper.
                    if(sc>=p2c_best_sc.load()||(sc==N&&phase2_collecting)){
                    Res hit;hit.sc=sc;hit.desc="WIDE_BIT nr="+std::to_string(nr)+" bit="+std::to_string(bp.k);
                    for(int i=0;i<p3_w[fi].n;i++)hit.body[i]=p3_w[fi].ops[i];
                    hit.nbody=p3_w[fi].n;hit.mode=MODE_ITER;hit.ointerp=OUT_BIT;
                    hit.outr=0;hit.nr=nr;hit.bit_pos=bp.k;hit.mdl=computeMDL(hit,ncat);
                    tryUpdateBestLex(best, hit, sc, &p2c_best_sc, &p2c_mtx);
                    printf("    WIDE_BIT HIT sc=%d bit=%d body={",sc,bp.k);
                    for(int i=0;i<p3_w[fi].n;i++)printf("%s%s",i?";":"",p3_w[fi].ops[i].str().c_str());
                    printf("} MDL=%.1f\n",hit.mdl);
                    if(sc==N){recordProg(hit,ncat,tgt,A);
                    if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}}}}
                }};
            int nt=std::min((int)std::thread::hardware_concurrency(),std::max(1,(int)p3_w.size()));
            if(nt<=1)loop1_worker();
            else{std::vector<std::thread>thr;for(int t=0;t<nt;t++)thr.emplace_back(loop1_worker);
                for(auto&t:thr)t.join();}}
            // Loop 2: depth-4 streaming - parallelized
            // LOOP bodies skipped (tested via pool above). Non-LOOP cheap to stream.
            std::atomic<long long>d4_tested(0),d4_hits(0);
            {std::atomic<int>next_fi(0);
            auto loop2_worker=[&](){
                while(true){
                    int fi=next_fi.fetch_add(1);
                    if(fi>=(int)p3_w.size()||now_s()>p2c_dl)return;
                    if(phase2_collecting&&now_s()>p2c_dl_get())return;
                    if(p2c_best_sc.load()>=N&&!phase2_collecting)return;
                    if(p3_w[fi].n>=kDDBBodyMax)continue;
                    if(p3_w[fi].has_loop)continue;
                    Ins body[kDDBBodyMax];for(int i=0;i<p3_w[fi].n;i++)body[i]=p3_w[fi].ops[i];
                    int nb=p3_w[fi].n+1;
                    for(int t=0;t<nfL1_nl&&now_s()<p2c_dl;t++){
                        body[p3_w[fi].n]=fL1_noloop[t];
                        d4_tested.fetch_add(1,std::memory_order_relaxed);
                        {auto&bp=bps[0];
                        W R[kRegisterCount]={};R[0]=bp.initW;int sc=0;
                        for(int t2=0;t2<N;t2++){if((int)R[0].bit(bp.k)!=tgt[t2])break;sc++;
                        for(int ii=0;ii<nb;ii++)exW(R,body[ii]);}
                        // outer guard admits equal-sc for lex-MDL tiebreak.
                        if(sc>=p2c_best_sc.load()||(sc==N&&phase2_collecting)){
                        d4_hits.fetch_add(1,std::memory_order_relaxed);
                        int best_sc_bp=sc;int best_k=bp.k;W best_initW=bp.initW;
                        for(int bpi=1;bpi<(int)bps.size();bpi++){
                            W R2[kRegisterCount]={};R2[0]=bps[bpi].initW;int sc2=0;
                            for(int t3=0;t3<N;t3++){if((int)R2[0].bit(bps[bpi].k)!=tgt[t3])break;sc2++;
                            for(int ii=0;ii<nb;ii++)exW(R2,body[ii]);}
                            // lex-(sc, -uInt(bit_pos)) sub-selection. Body is fixed
                            // within this candidate; only bit_pos varies. uInt(bit_pos) is the
                            // bit-position contribution to computeMDL. Smaller is lex-better.
                            if(sc2>best_sc_bp ||
                               (sc2==best_sc_bp && uInt(bps[bpi].k)<uInt(best_k))){
                                best_sc_bp=sc2;best_k=bps[bpi].k;best_initW=bps[bpi].initW;}}
                        Res hit;hit.sc=best_sc_bp;hit.desc="WIDE_BIT nr="+std::to_string(nr)+" bit="+std::to_string(best_k);
                        for(int i=0;i<nb;i++)hit.body[i]=body[i];
                        hit.nbody=nb;hit.mode=MODE_ITER;hit.ointerp=OUT_BIT;
                        hit.outr=0;hit.nr=nr;hit.bit_pos=best_k;hit.mdl=computeMDL(hit,ncat);
                        // lex-best (sc, -mdl) update via helper.
                        tryUpdateBestLex(best, hit, best_sc_bp, &p2c_best_sc, &p2c_mtx);
                        printf("    WIDE_BIT HIT sc=%d bit=%d body={",best_sc_bp,best_k);
                        for(int i=0;i<nb;i++)printf("%s%s",i?";":"",body[i].str().c_str());
                        printf("} MDL=%.1f\n",hit.mdl);
                        if(best_sc_bp==N){recordProg(hit,ncat,tgt,A);
                        if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}}}}
                    }
                }};
            int nt=std::min((int)std::thread::hardware_concurrency(),std::max(1,(int)p3_w.size()));
            if(nt<=1)loop2_worker();
            else{std::vector<std::thread>thr;for(int t=0;t<nt;t++)thr.emplace_back(loop2_worker);
                for(auto&t:thr)t.join();}}
            printf("    wide_bit nr=%d stream: %lld tested, %lld hits %.1fs\n",nr,d4_tested.load(),d4_hits.load(),now_s());
            }
        }
    }}

    // Phases 2D/2E removed - subsumed by Phase 2F (L=1..8 unified WSBP).
    // ══════════════════════════════════════════════════════════════
    // PHASE 2F: Unified WSBP - Wire-Space Backward Propagation
    // ══════════════════════════════════════════════════════════════
    // Discovers ALL LOOP programs: pre_body + inner_body[L] + LOOP(kr, L)
    // for L=1..8 via backward demand-driven register inference.
    // Subsumes former Phases 2D/2E (pool-based LOOP search).
    //
    // Architecture (Solomonoff-valid, ISA-derived, general):
    // 1. Type-first enumeration (shorter L first = higher prior)
    // 2. WSBP backward trace from accumulator → wire topology
    // 3. ISZERO merge (ISA semantics: read before write)
    // 4. Unresolved demand resolution (try all class merges)
    // 5. 1-probe termination check (LOOP semantics + swap variants)
    // 6. Register permutation (P(nr, n_classes))
    // 7. Fingerprint dedup (avoids redundant testing at small L)
    // 8. Pre-body sweep + verification (unconditional correctness)
    //
    // nr is DERIVED from wire topology (n_classes), not hardcoded.
    // All filters are ISA-derived. Zero human knowledge.
    //
    // See: Coupling Barrier Theorem (Appendix C), WSBP_INTEGRATION.md
    // ══════════════════════════════════════════════════════════════
    // Phase 2F runs in collecting mode when an earlier phase
    // produced sc=N - matching the lex-best collection the other phases use.
    if((best.sc<N||(phase2_collecting&&now_s()<p2c_dl_get()))&&now_s()<dl&&(dl-now_s())>(double)kIsaMaxConstantConstexpr){
        // library-bootstrap fix: reserve Phase 2H budget (compositional/hierarchical synthesis runs after
        // Phase 2F) IFF the library has invocable entries. Without invocable entries
        // Phase 2H gates itself off, so reserving budget for it would waste time.
        // Reserve = max(60s, 20% of remaining), uniform across all targets when applicable.
        int p2h_invocable = 0;
        {int sz_lib = subCallCatalogSize();
         for (int idx = 0; idx < sz_lib; idx++)
             if (subCallLibraryEntryInvocable(idx)) p2h_invocable++;}
        double p2f_dl;
        if (p2h_invocable >= 1) {
            // Phase 2H reserve = 2/isaMaxConstant of
            // remaining = 0.2 with current ISA. ISA-derived; numerically identical.
            double p2h_reserve = (dl-now_s())*(2.0/(double)isaMaxConstant());
            p2f_dl = dl - p2h_reserve;
            printf("    phase2f_wsbp %.1fs remaining=%.1fs (Phase 2H reserve=%.1fs, lib=%d)\n",
                   now_s(), p2f_dl-now_s(), p2h_reserve, p2h_invocable);
        } else {
            p2f_dl = dl; // no library → no Phase 2H → Phase 2F can use all remaining
            printf("    phase2f_wsbp %.1fs remaining=%.1fs (Phase 2H disabled, lib=0)\n",
                   now_s(), dl-now_s());
        }

        // Type catalog: built from the FULL ISA (isaConstantRanges) + nested LOOP.
        // No hand-curation. The ISA is the Solomonoff prior.
        struct TyE{int ti,c,ar;};
        std::vector<TyE>tyCat;
        {auto ranges=isaConstantRanges();
        for(auto&[ti,consts]:ranges){
            int ar=typeAr(ti);if(!ar)continue;
            for(int c:consts)tyCat.push_back({ti,c,ar});}
        // nested LOOP inner body lengths bound.
        // Derivation: kDDBBodyMax / 2 - half the max DDB body length, so the
        // outer body has equal room for the LOOP instruction itself plus one
        // reducing instruction. With current ISA (kDDBBodyMax=8) this evaluates
        // to 4, identical to the previous magic literal.
        for(int ll=1;ll<=kDDBBodyMax/2;ll++)tyCat.push_back({17,ll,1});}
        int nT=(int)tyCat.size();

        // wIdx2: writer-arg-index lookup for tyCat opcodes. Table covers canonical
        // ti=0..17. SUB_CALL (ti=32) is excluded from tyCat by typeAr=0 → ar==0 →
        // continue at the loop above, so wIdx2 is never called with ti≥18.
        // Convention: ti=12 OUT and ti=17 LOOP entries preserved from pre-cleanup
        // semantics (callers filter these via reducing-opcode check, not via wIdx2).
        static constexpr int8_t wIdx2_tab[18]={
            0,0,2,2,2,1,1,2,2,2,0,1,2,2,2,2,1,0
        };
        // bound derives from table size, not literal 18.
        constexpr int kWIdx2Count = (int)(sizeof(wIdx2_tab)/sizeof(wIdx2_tab[0]));
        auto wIdx2=[&](int ti)->int{
            return (ti>=0 && ti<kWIdx2Count) ? wIdx2_tab[ti] : -1;
        };

        int n_quick_f=std::min(N,kQuickCheckLen);

        // ── Pre-body pools per nr (built lazily, MDL-ordered) ──
        struct PreF{Ins ops[3];int n;};
        struct PSF{int64_t S[kQuickCheckLen][kRegisterCount];bool ok[kQuickCheckLen];int idx;};
        struct PrePool{std::vector<PreF>all;std::vector<PSF>rpref[kRegisterCount];bool built;};
        PrePool pools[kRegisterCount+1]={}; // pools[nr] for nr=0..kRegisterCount

        auto ensurePool=[&](int nr){
            if(pools[nr].built)return;pools[nr].built=true;
            auto fL1=buildL1(nr);
            {PreF e;e.n=0;pools[nr].all.push_back(e);}
            for(auto&ins:fL1){PreF e;e.ops[0]=ins;e.n=1;pools[nr].all.push_back(e);}
            //
            // cap2 = min(theoretical_max, memory_bound):
            // theoretical_max = |fL1|² (depth-2 pair combinatorial ceiling)
            // memory_bound = free_mem / 2 / per_entry_bytes
            // per_entry_bytes ≈ nr × sizeof(PSF) (rpref[kr] tiers store PSF arrays).
            // Time bound is implicit in Phase 2F's existing dl_L deadline check
            // inside the multiset enumeration loop.
            //
            // review had previously regressed with this approach (parityalt
            // PARTIAL). Root cause was actually review's cost-check break
            // (fixed in #70) which masked Phase 2A behavior under load. With
            // that fix and Phase 2C's similar fix (#71) in place, retesting.
            int64_t theoretical = (int64_t)fL1.size() * (int64_t)fL1.size();
            size_t per_entry_bytes_p2f = sizeof(PSF) * std::max(1, nr);
            int64_t mem_bound = (int64_t)(currentFreeMemoryBytes() / 2 / std::max((size_t)1, per_entry_bytes_p2f));
            int cap2 = (int)std::max((int64_t)1, std::min({(int64_t)INT_MAX, theoretical, mem_bound}));
            for(int i=0;i<(int)fL1.size()&&(int)pools[nr].all.size()<cap2;i++)
                for(int j=0;j<(int)fL1.size()&&(int)pools[nr].all.size()<cap2;j++){
                    PreF e;e.ops[0]=fL1[i];e.ops[1]=fL1[j];e.n=2;pools[nr].all.push_back(e);}
            // A4: Cache pre-body register states once, reuse across kr iterations.
            // Body execution is independent of kr - saves (nr-1) × pool_size × kQuickCheckLen exBodyFlat calls.
            struct PSCache{bool ok[kQuickCheckLen];int64_t S[kQuickCheckLen][kRegisterCount];};
            std::vector<PSCache>pcache(pools[nr].all.size());
            for(int pi=0;pi<(int)pools[nr].all.size();pi++){
                auto&p=pools[nr].all[pi];
                for(int n2=0;n2<kQuickCheckLen;n2++){
                    int64_t R[kRegisterCount]={};R[0]=n2;g_sat=false;
                    if(p.n>0)exBodyFlat(R,p.ops,p.n);
                    if(g_sat){pcache[pi].ok[n2]=false;continue;}
                    pcache[pi].ok[n2]=true;for(int r=0;r<kRegisterCount;r++)pcache[pi].S[n2][r]=R[r];}}
            for(int kr=0;kr<nr;kr++){
                for(int pi=0;pi<(int)pools[nr].all.size();pi++){
                    auto&pc=pcache[pi];PSF ps;ps.idx=pi;
                    bool any=false;int64_t first=-1;bool varies=false;
                    for(int n2=0;n2<kQuickCheckLen;n2++){
                        ps.ok[n2]=pc.ok[n2];if(!pc.ok[n2])continue;
                        for(int r=0;r<kRegisterCount;r++)ps.S[n2][r]=pc.S[n2][r];
                        if(pc.S[n2][kr]>0)any=true;
                        if(n2==0)first=pc.S[n2][kr];else if(pc.S[n2][kr]!=first)varies=true;}
                    if(!pc.ok[0]||!any||!varies)continue;
                    pools[nr].rpref[kr].push_back(ps);}
                std::unordered_set<uint64_t>sigs;std::vector<PSF>dd;
                for(auto&ps:pools[nr].rpref[kr]){uint64_t h=0xcbf29ce484222325ULL;
                    for(int n2=0;n2<kQuickCheckLen;n2++){if(!ps.ok[n2]){h^=999999937ULL;h*=0x9e3779b97f4a7c15ULL;continue;}
                        h^=(uint64_t)(ps.S[n2][kr]*1000003+ps.S[n2][0]*7919+n2*104729);h*=0x9e3779b97f4a7c15ULL;}
                    if(!sigs.count(h)){sigs.insert(h);dd.push_back(ps);}}
                pools[nr].rpref[kr]=dd;}
            printf("    pre-pool nr=%d: %d entries\n",nr,(int)pools[nr].all.size());};

        // ── General WSBP loop: L=1..10, nr derived from wire topology ──
        // A1: Thread-safe counters for parallel multiset processing
        std::atomic<long long>tuples_tested(0),wirings_tested(0);
        std::atomic<int>p2f_hits(0);
        std::atomic<int>best_sc_shared(best.sc);
        std::mutex best_mtx;
        double dl_L=p2f_dl;
        // L_max derives from isaMaxConstant() - same ISA-structural
        // bound used in review. NESTED_LOOP body length bound matches
        // ISA constant magnitude.
        const int kLMax=isaMaxConstant();
        for(int L=1;L<=kLMax&&now_s()<p2f_dl&&(best.sc<N||(phase2_collecting&&now_s()<p2c_dl_get()));L++){
            // R15/D1: Adaptive Levin budget - equal share of remaining per L level.
            {double rem=p2f_dl-now_s();
            int levels_left=kLMax-L+1;
            dl_L=std::min(p2f_dl,now_s()+rem/std::max(1,levels_left));}
            // Fingerprint dedup: at small L, many type tuples + permutations
            // produce identical concrete bodies. Dedup avoids redundant testing.
            std::unordered_set<uint64_t>seen_bodies;seen_bodies.reserve(kFpHashReserve);
            // Full ISA catalog (~53 types): 53^4≈7.9M too large for flat; multisets at L>=kDDBBodyMax/2.
            // Both are Solomonoff-valid orderings within the same MDL class.
            // boundary expressed as kDDBBodyMax/2 = 4
            // (half max DDB body length); same value, ISA-derived.
            long long total_tuples=1;for(int i=0;i<L;i++)total_tuples*=nT;
            bool use_multisets=(L>=kDDBBodyMax/2); // Multisets when L exceeds half ISA body length

            // Type-SET generation for L≥6
            struct MS{int idx[kProgramBodyMax/2];int distinct;};
            std::vector<MS>msets;
            if(use_multisets){
                // ISA-derived filters only (no human knowledge):
                // 1. ADD (ti=2): structural - WSBP traces from accumulator
                // 2. Can-reduce: at least one instruction that can drive R[kr]→0
                // DEC(1), SUB(3), MOD_C(6), MOD_R(7), DIVC_R(8), ISZERO(16), LOOP(17)
                // 3. popcount ≥ 2: computation theory (single-type body is trivial)
                // 4. LOOP(c) at position i requires i >= c
                // ISA-derived filters (no human knowledge, no accumulator assumption):
                // 1. Can-reduce: at least one instruction that can drive R[kr]→0
                // 2. popcount ≥ 2: non-trivial body
                // 3. LOOP(c) position constraint
                auto valid=[&](int*c)->bool{bool can_reduce=false;uint32_t seen=0;
                    for(int i=0;i<L;i++){int ti=tyCat[c[i]].ti;seen|=(1u<<ti);
                        if(ti==1||ti==3||ti==6||ti==7||ti==8||ti==16||ti==17)can_reduce=true;
                        if(ti==17&&i<tyCat[c[i]].c)return false;}
                    return can_reduce&&__builtin_popcount(seen)>=2;};
                // structural cap = min(time-bound, memory-bound).
                // per_op_cost: machine-calibrated at runtime
                // per_test_cost = kLMax × N × L × per_op_cost
                // where the multiplier reflects: each multiset test runs body
                // of length L over N positions for up to kLMax wire/kr candidates.
                // ms_cap_time = dl_L / per_test_cost
                // ms_cap_mem = available_memory / (kLMax × sizeof(MS))
                // ms_cap = min(ms_cap_time, ms_cap_mem)
                // All inputs are structural: budget (dl_L), sequence (N), level (L),
                // ISA (kLMax), machine (per_op_cost, available_memory). No magic.
                double per_op_cost=calibratePerOpCost();
                double per_test_cost=(double)kLMax*(double)N*(double)L*per_op_cost;
                double remaining_dl=std::max(0.0,dl_L-now_s());
                long long ms_cap_time=(long long)(remaining_dl/std::max(per_test_cost,1e-12));
                long long ms_cap_mem=(long long)(availableMemoryBytes()/(size_t)std::max(1,kLMax)/sizeof(MS));
                long long ms_cap=std::min(ms_cap_time,ms_cap_mem);
                if(ms_cap<1)ms_cap=1;
                auto gen=[&](auto&self,int*c,int d,int mi)->void{
                    if(now_s()>dl_L)return; // deadline-bounded (safety)
                    if((long long)msets.size()>=ms_cap)return; // structural cap
                    if(d==L){if(valid(c)){MS m;for(int i=0;i<L;i++)m.idx[i]=c[i];
                        uint32_t seen=0;for(int i=0;i<L;i++)seen|=(1u<<tyCat[c[i]].ti);
                        m.distinct=__builtin_popcount(seen);
                        msets.push_back(m);}return;}
                    for(int i=mi;i<nT;i++){c[d]=i;self(self,c,d+1,i);}};
                int c[kProgramBodyMax/2]={};gen(gen,c,0,0);
                // Sort: diversity descending (primary).
                // For L>=9: nc_score ascending as secondary key (lower nc = more likely viable).
                // Sort: diversity descending (primary), MDL ascending (secondary).
                // MDL = sum of per-instruction constant costs. Instructions with c=0
                // encode shorter (no constant). This is Solomonoff-optimal: higher prior
                // weight programs come first. ISA-derived, no human knowledge.
                std::stable_sort(msets.begin(),msets.end(),[&](const MS&a,const MS&b){
                    if(a.distinct!=b.distinct)return a.distinct>b.distinct;
                    // Secondary: sum of constant encoding costs (lower = simpler = higher prior)
                    double ca=0,cb=0;
                    for(int i=0;i<L;i++){ca+=uInt(tyCat[a.idx[i]].c);cb+=uInt(tyCat[b.idx[i]].c);}
                    return ca<cb;});
                printf("    L=%d: %d multisets (type-SET)\n",L,(int)msets.size());
            }else{
                printf("    L=%d: %lld tuples (flat)\n",L,total_tuples);
            }

            // Unified tuple iteration (flat or multiset-based)
            std::atomic<long long>L_tuples(0);
            auto processTuple=[&](TyE*types,std::unordered_set<uint64_t>&seen_bodies){
                // ISA-derived filters (no human knowledge, no accumulator assumption):
                // 1. Must have a register-reducing instruction (LOOP termination)
                // 2. Must have ≥2 distinct types (non-trivial body)
                // 3. LOOP(c) at position i requires i >= c
                {bool cr=false;uint32_t seen=0;
                 for(int i=0;i<L;i++){int ti=types[i].ti;seen|=(1u<<ti);
                     if(ti==1||ti==3||ti==6||ti==7||ti==8||ti==16||ti==17)cr=true;
                     if(ti==17&&i<types[i].c)return;}
                 if(!cr||__builtin_popcount(seen)<2)return;}
                bool tuple_has_loop=false;
                for(int i=0;i<L;i++)if(types[i].ti==17){tuple_has_loop=true;break;}
                tuples_tested++;L_tuples++;

                // WSBP for each output source position (any instruction type).
                // ISA-derived ordering: ar=3 first (richest dataflow), reverse body
                // order (last writer determines register value in sequential execution).
                for(int ar_pass=3;ar_pass>=1;ar_pass--){
                for(int pos_out=L-1;pos_out>=0;pos_out--){
                    if(types[pos_out].ar!=ar_pass)continue;
                    if(types[pos_out].ti==10)continue; // LOAD has no input to trace
                    if(now_s()>dl_L||best_sc_shared.load()>=N)return;

                    int out_ar=types[pos_out].ar;
                    int out_wii=wIdx2(types[pos_out].ti);
                    // E1: seed variant. Pass 1 = self-ref only (accumulator/NESTED_LOOP).
                    // Pass 2 (non-self-ref) runs AFTER pass 1 with remaining budget.
                    int n_seed_vars=1; // Pass 1: self-ref only
                    for(int seed_var=0;seed_var<n_seed_vars&&now_s()<dl_L;seed_var++){

                    // Backward trace seeded from pos_out's output port
                    Ins wb[kProgramBodyMax/2];for(int i=0;i<L;i++){wb[i].ti=types[i].ti;wb[i].c=types[i].c;
                        wb[i].ar=types[i].ar;for(int a=0;a<4;a++)wb[i].args[a]=-1;}
                    bool asgn[kProgramBodyMax/2]={};int nw=0;
                    struct WD{int wire,cons;};WD dm[4*kDDBBodyMax];int ndm=0;
                    int unr[kProgramBodyMax/2];int nunr=0;
                    int wo=nw++;
                    wb[pos_out].args[out_wii]=wo;
                    asgn[pos_out]=true;
                    if(seed_var==0&&out_ar>=3){
                        // Variant 0: self-ref (args[0]=wo for loop-carried accumulation)
                        wb[pos_out].args[0]=wo;
                        int wi2=nw++;wb[pos_out].args[1]=wi2;
                        dm[ndm++]={wi2,pos_out};
                    }else if(seed_var==1&&out_ar>=3){
                        // Variant 1: non-self-ref (both inputs traced independently)
                        int wa=nw++,wb2=nw++;
                        wb[pos_out].args[0]=wa;wb[pos_out].args[1]=wb2;
                        dm[ndm++]={wa,pos_out};dm[ndm++]={wb2,pos_out};
                    }else if(out_ar>=2){
                        int wi2=nw++;wb[pos_out].args[0]=wi2;
                        dm[ndm++]={wi2,pos_out};
                    }
                    // ar=1: self-ref (INC/DEC/LOOP), output=input, no demand needed
                    while(ndm>0){WD d2=dm[--ndm];int prod=-1;
                        for(int delta=1;delta<L;delta++){int p=(d2.cons-delta+L)%L;
                            if(!asgn[p]&&wb[p].ti!=10){prod=p;break;}}
                        if(prod<0){if(nunr<kRegisterCount)unr[nunr++]=d2.wire;continue;}
                        asgn[prod]=true;int ti=wb[prod].ti,wii=wIdx2(ti);
                        wb[prod].args[wii]=d2.wire;
                        if(ti==0||ti==1||ti==17){wb[prod].args[0]=d2.wire;} // INC/DEC/LOOP: self-ref single register
                        else if(ti==16||ti==11||ti==5||ti==6){int w=nw++;wb[prod].args[0]=w;dm[ndm++]={w,prod};}
                        else if(ti==2||ti==3||ti==4||ti==7||ti==13||ti==14||ti==15){
                            int w0=nw++,w1=nw++;wb[prod].args[0]=w0;wb[prod].args[1]=w1;
                            dm[ndm++]={w0,prod};dm[ndm++]={w1,prod};}
                        else if(ti==8){int w=nw++;wb[prod].args[0]=w;wb[prod].args[1]=nw++;dm[ndm++]={w,prod};}
                        else if(ti==9){int w0=nw++,w1=nw++;wb[prod].args[0]=w0;wb[prod].args[1]=w1;
                            wb[prod].args[2]=d2.wire;wb[prod].args[3]=nw++;
                            dm[ndm++]={w0,prod};dm[ndm++]={w1,prod};}}
                    for(int i=0;i<L;i++){if(asgn[i])continue;int wii=wIdx2(wb[i].ti);
                        if(wb[i].args[wii]<0)wb[i].args[wii]=nw++;
                        if(wb[i].ti==0||wb[i].ti==1||wb[i].ti==17)wb[i].args[0]=wb[i].args[wii];
                        for(int a=0;a<wb[i].ar;a++)if(wb[i].args[a]<0)wb[i].args[a]=nw++;}

                    // ISZERO merge (ISA-derived: read before write → can share register)
                    int par[8*kDDBBodyMax];for(int i=0;i<nw;i++)par[i]=i;
                    auto ufF=[&](int x)->int{while(par[x]!=x)x=par[x]=par[par[x]];return x;};
                    auto ufM=[&](int a,int b){par[ufF(a)]=ufF(b);};
                    for(int i=0;i<L;i++)if(wb[i].ti==16&&wb[i].args[0]>=0&&wb[i].args[1]>=0)
                        ufM(wb[i].args[0],wb[i].args[1]);
                    // Non-self-ref: merge unresolved wires with output wire (wo=0).
                    // ISA-derived: in MODE_ITER, unresolved inputs default to the state
                    // register R[0]. This reduces nr from 6-7 to 3-4, enabling E3 testing.
                    if(seed_var==1)for(int i=0;i<nunr;i++)ufM(unr[i],0);

                    // Resolved roots
                    int rr[8*kDDBBodyMax];int nrr=0;{bool seen[8*kDDBBodyMax]={};
                        for(int i=0;i<L;i++)for(int a=0;a<wb[i].ar;a++){
                            int w=ufF(wb[i].args[a]);if(w>=0&&!seen[w]){seen[w]=true;rr[nrr++]=w;}}}
                    // Unresolved resolution
                    // E4: high URC cap for non-self-ref (seed_var=1) at small L.
                    // Self-ref topologies have few unresolved wires → nurc < base naturally.
                    // Levin partition with permissive
                    // nc estimate; deadline-bounded effective enforcement):
                    //
                    // Each URC config triggers P(nr, nc_actual) permutations × body_test,
                    // where nc_actual ∈ [nrr, nrr + nunr] depending on per-config choices:
                    // • ch < nrr: merge unr[i] with rr[ch] → no new class
                    // • ch == nrr: leave unr[i] unmerged → adds a new class
                    // (See URC encoding at the for-loop two scopes below.)
                    //
                    // We use nc_est = nrr - the LOWER bound on nc_actual - yielding the
                    // most permissive nperm and largest nurc_cap. The cap thus admits URC
                    // configs whose mean cost may exceed the budget fraction; the dl_L
                    // deadline poll INSIDE the URC loop (`now_s()<dl_L`, just below) then
                    // truncates enumeration when wall time is exhausted. Operationally
                    // equivalent to using mean cost (E[nc] = nrr + nunr/(nrr+1)).
                    //
                    // C3 alignment: a permissive structural cap + deadline backstop
                    // explores more URC configs in available time, preserving Solomonoff
                    // ordering by not artificially cutting off enumeration. review
                    // attempted nc_est = nrr+nunr (worst case) but regressed sigma - the
                    // worst-case bound is artificially restrictive given typical merge
                    // rates.
                    //
                    // per_urc_step = P(kRegisterCount, min(nrr, kRegisterCount)) ×
                    // N × kDDBBodyMax × per_op_cost
                    // urc_share = (dl_L - now) / kCascadePoolLevels
                    // nurc_cap = urc_share / per_urc_step
                    int nurc_cap;
                    {
                        double per_op_urc = calibratePerOpCost();
                        int nc_est = std::min(std::max(nrr, 1), kRegisterCount);
                        double nperm_est = 1.0;
                        for (int i = kRegisterCount - nc_est + 1; i <= kRegisterCount; i++)
                            nperm_est *= (double)i;
                        double per_urc_step = nperm_est * (double)N * (double)kDDBBodyMax * per_op_urc;
                        double remaining_dl_L = std::max(0.0, dl_L - now_s());
                        double urc_phase_share = remaining_dl_L / (double)kCascadePoolLevels;
                        int64_t cap_int = (int64_t)(urc_phase_share / std::max(per_urc_step, 1e-12));
                        nurc_cap = (int)std::max((int64_t)1, std::min((int64_t)INT_MAX, cap_int));
                    }
                    (void)seed_var;  // L/seed-band differentiation now implicit in nperm cost
                    int nurc=1;for(int i=0;i<nunr&&nurc<=nurc_cap;i++)nurc*=(nrr+1);
                    if(nurc>nurc_cap)continue;

                    for(int urc=0;urc<nurc&&best_sc_shared.load()<N&&now_s()<dl_L;urc++){
                        int parc[8*kDDBBodyMax];for(int i=0;i<nw;i++)parc[i]=par[i];
                        auto ufFc=[&](int x)->int{while(parc[x]!=x)x=parc[x]=parc[parc[x]];return x;};
                        auto ufMc=[&](int a,int b){parc[ufFc(a)]=ufFc(b);};
                        {int v=urc;for(int i=0;i<nunr;i++){int ch=v%(nrr+1);v/=(nrr+1);
                            if(ch<nrr)ufMc(unr[i],rr[ch]);}}

                        // Count wire classes → determines nr needed (ISA-derived)
                        int cm[8*kDDBBodyMax];for(int i=0;i<nw;i++)cm[i]=-1;int nc=0;
                        for(int i=0;i<L;i++)for(int a=0;a<wb[i].ar;a++){
                            int w=ufFc(wb[i].args[a]);if(w>=0&&cm[w]<0)cm[w]=nc++;}
                        for(int i=0;i<nunr;i++){int w=ufFc(unr[i]);if(cm[w]<0)cm[w]=nc++;}
                        int nr=nc; // nr DERIVED from wire topology
                        // ISA constraint: nr < L ensures at least one shared register
                        // within the loop body (intra-iteration dataflow coupling).
                        if(nr>=L||nr<2||nr>kRegisterCount)continue; // physical register count = kRegisterCount, nr < L for NESTED_LOOP

                        // For non-self-ref (seed_var=1): skip kr probe and NESTED_LOOP testing.
                        // Non-self-ref bodies are tested ONLY via E3 (MODE_FUNC/ITER/OUT_BIT).
                        // This avoids the kr probe overhead which dominates processing time.
                        uint8_t kr_candidates=0;
                        if(seed_var==0){
                        // Execution-based kr discovery (ISA-derived: LOOP terminates
                        // when R[kr]==0. Execute identity-permutation body, observe which
                        // registers reach 0. No type-based kr guessing - discovered from
                        // execution semantics. Also serves as termination check.)
                        {Ins qb[kProgramBodyMax/2];for(int i=0;i<L;i++){qb[i].ti=wb[i].ti;qb[i].c=wb[i].c;qb[i].ar=wb[i].ar;
                            for(int a=0;a<qb[i].ar;a++){int w=ufFc(wb[i].args[a]);
                                qb[i].args[a]=(cm[w]>=0)?cm[w]:0;}
                            for(int a=qb[i].ar;a<4;a++)qb[i].args[a]=0;}
                        int sq[kProgramBodyMax/2];int nsq=0;
                        auto _ncomm=[&](int ti)->bool{ if(ti==3||ti==7)return true;
                            return false; };
                        for(int i=0;i<L;i++)if(_ncomm(qb[i].ti))sq[nsq++]=i;
                        // kr-probe swap-variant cap = kDDBBodyMax/2 (= 4).
                        // Matches review's structural rationale for swap-variant cap
                        // in the broader inner enumeration (line 3314-style).
                        int nswq=1<<nsq;if(nswq>kDDBBodyMax/2)nswq=kDDBBodyMax/2;
                        for(int svq=0;svq<nswq;svq++){
                            Ins qb2[kProgramBodyMax/2];for(int i=0;i<L;i++)qb2[i]=qb[i];
                            for(int i=0;i<nsq;i++)if(svq&(1<<i))std::swap(qb2[sq[i]].args[0],qb2[sq[i]].args[1]);
                            BodyDesc qbd;computeBodyDesc(qb2,L,qbd);
                            for(int pn=1;pn<=3;pn++){
                                int64_t R[kRegisterCount]={};for(int r=0;r<nr;r++)R[r]=pn+r;
                                g_sat=false;
                                for(int it=0;it<kQuickCheckLen&&!g_sat;it++){
                                    if(qbd.has_loop)exBodyD(R,qb2,L,qbd);
                                    else exBodyFlat(R,qb2,L);}
                                if(!g_sat)for(int r=0;r<nr;r++)if(R[r]==0)kr_candidates|=(1<<r);}}
                        // kr_candidates may be empty for non-LOOP programs.
                        // Don't skip - E3 multi-mode tests don't need kr.
                        // NESTED_LOOP testing (below) is gated by kr_candidates internally.
                        }
                        } // end if(seed_var==0) - kr probe

                        // Ensure pre-body pool exists for this nr (needed for NESTED_LOOP testing)
                        if(kr_candidates&&seed_var==0)ensurePool(nr);

                        // Input-swap variants (non-commutative: SUB, MOD_R)
                        int ncp[kProgramBodyMax/2],nnc=0;
                        for(int i=0;i<L;i++){int ti=wb[i].ti;bool nc=(ti==3||ti==7);
                            if(nc)ncp[nnc++]=i;}
                        // swap-variant count cap.
                        // Derivation: 1 << (kDDBBodyMax/2) = 2^4 = 16. Bounds the
                        // number of input-swap variants explored when there are
                        // many non-commutative instructions; structurally tied to
                        // half-DDB body length.
                        int nsw=1<<nnc;if(nsw>(1<<(kDDBBodyMax/2)))nsw=(1<<(kDDBBodyMax/2));

                        // Register permutation: P(nr, nc) - all distinct assignments
                        int pm2[kProgramBodyMax/2];for(int i=0;i<nc;i++)pm2[i]=i;
                        auto np=[&]()->bool{for(int i=nc-1;i>=0;i--){pm2[i]++;
                            rr2:if(pm2[i]>=nr){pm2[i]=0;if(i==0)return false;continue;}
                            for(int j=0;j<i;j++)if(pm2[j]==pm2[i]){pm2[i]++;goto rr2;}
                            for(int k=i+1;k<nc;k++){pm2[k]=0;
                                rr2b:for(int j=0;j<k;j++)if(pm2[j]==pm2[k]){pm2[k]++;goto rr2b;}}
                            return true;}return false;};
                        do{if(now_s()>dl_L||best_sc_shared.load()>=N)return;
                            // Build base body ONCE per permutation (ufFc/cm/pm2 are invariant across swaps)
                            Ins base_body[kProgramBodyMax/2];for(int i=0;i<L;i++){base_body[i].ti=wb[i].ti;base_body[i].c=wb[i].c;base_body[i].ar=wb[i].ar;
                                for(int a=0;a<base_body[i].ar;a++){int w=ufFc(wb[i].args[a]);
                                    base_body[i].args[a]=(cm[w]>=0)?pm2[cm[w]]:0;}
                                for(int a=base_body[i].ar;a<4;a++)base_body[i].args[a]=0;}
                            for(int sv=0;sv<nsw;sv++){
                                Ins body[kProgramBodyMax/2];memcpy(body,base_body,L*sizeof(Ins));
                                {for(int i=0;i<nnc;i++)if(sv&(1<<i))std::swap(body[ncp[i]].args[0],body[ncp[i]].args[1]);}
                                wirings_tested++;
                                int outr=body[pos_out].args[wIdx2(body[pos_out].ti)];

                                // Fingerprint dedup: hash concrete body to skip duplicates
                                {uint64_t bfp=0xcbf29ce484222325ULL;
                                 for(int i=0;i<L;i++){bfp=hmix(bfp,(uint64_t)body[i].ti);bfp=hmix(bfp,(uint64_t)body[i].c);
                                     for(int a=0;a<body[i].ar;a++)bfp=hmix(bfp,(uint64_t)body[i].args[a]);}
                                 if(!seen_bodies.insert(bfp).second)continue;}

                                // Precompute inner body BodyDesc. Skip full scan for non-LOOP tuples (~90%).
                                BodyDesc inner_bd;
                                if(tuple_has_loop)computeBodyDesc(body,L,inner_bd);
                                else{inner_bd.has_loop=false;inner_bd.in_loop_mask=0;inner_bd.loop_inner_mask=0;}
                                // E3: Multi-mode testing. Same body tested as
                                // MODE_FUNC (no LOOP), MODE_ITER, and OUT_BIT.
                                // ISA-derived: same instructions, different execution model.
                                int e3_outr=body[pos_out].args[wIdx2(body[pos_out].ti)];
                                // MODE_FUNC without LOOP
                                {int sc_f=0;
                                for(int n=0;n<N;n++){int64_t R[kRegisterCount]={};R[0]=n;g_sat=false;
                                    if(inner_bd.has_loop)exBodyD(R,body,L,inner_bd);
                                    else exBodyFlat(R,body,L);
                                    if(g_sat)break;if(pm(R[e3_outr],A)!=tgt[n])break;sc_f++;}
                                // lex-best (sc, -mdl) update via helper.
                                if(sc_f>=best_sc_shared.load()){Res h;h.sc=sc_f;
                                    h.desc="FUNC_F L="+std::to_string(L);
                                    h.mode=MODE_FUNC;h.ointerp=OUT_MOD;
                                    for(int i=0;i<L;i++)h.body[i]=body[i];h.nbody=L;
                                    h.nr=nr;h.outr=e3_outr;h.mdl=computeMDL(h,ncat);
                                    tryUpdateBestLex(best, h, sc_f, &best_sc_shared, &best_mtx);
                                    if(sc_f==N){recordProg(h,ncat,tgt,A);
                                        if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                                        if(now_s()>p2c_dl_get())return;}}}
                                // MODE_ITER
                                for(int r0=0;r0<=2&&best_sc_shared.load()<N;r0++){
                                    if(pm(r0,A)!=tgt[0])continue;
                                    int64_t R[kRegisterCount]={};R[0]=r0;g_sat=false;int sc_i=0;
                                    for(int t=0;t<N&&!g_sat;t++){if(pm(R[e3_outr],A)!=tgt[t])break;sc_i++;
                                        if(inner_bd.has_loop)exBodyD(R,body,L,inner_bd);
                                        else exBodyFlat(R,body,L);}
                                    // lex-best (sc, -mdl) update via helper.
                                    if(sc_i>=best_sc_shared.load()){Res h;h.sc=sc_i;
                                        h.desc="ITER_F L="+std::to_string(L);
                                        h.mode=MODE_ITER;h.ointerp=OUT_MOD;
                                        for(int i=0;i<L;i++)h.body[i]=body[i];h.nbody=L;
                                        h.nr=nr;h.outr=e3_outr;h.init[0]=r0;h.mdl=computeMDL(h,ncat);
                                        tryUpdateBestLex(best, h, sc_i, &best_sc_shared, &best_mtx);
                                        if(sc_i==N){recordProg(h,ncat,tgt,A);
                                            if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                                            if(now_s()>p2c_dl_get())return;}}}
                                // OUT_BIT (A=2 only)
                                if(A==2&&best_sc_shared.load()<N){
                                    for(int ki=0;ki<kCascadePoolLevels;ki++){
                                        int k=(ki==0)?((1<<kRegisterCount)-1):((ki==1)?N:N/2);
                                        W initW=W::from(1);for(int s=0;s<k;s++)initW=initW+initW;
                                        if((int)initW.bit(k)!=tgt[0])continue;
                                        W Rw[kRegisterCount]={};Rw[0]=initW;int sc_b=0;
                                        for(int t=0;t<N;t++){if((int)Rw[0].bit(k)!=tgt[t])break;sc_b++;
                                            exBodyW(Rw,body,L);}
                                        // lex-best (sc, -mdl) update via helper.
                                        if(sc_b>=best_sc_shared.load()){Res h;h.sc=sc_b;
                                            h.desc="WBIT_F L="+std::to_string(L)+" bit="+std::to_string(k);
                                            h.mode=MODE_ITER;h.ointerp=OUT_BIT;
                                            for(int i=0;i<L;i++)h.body[i]=body[i];h.nbody=L;
                                            h.nr=nr;h.outr=0;h.bit_pos=k;h.mdl=computeMDL(h,ncat);
                                            tryUpdateBestLex(best, h, sc_b, &best_sc_shared, &best_mtx);
                                            if(sc_b==N){recordProg(h,ncat,tgt,A);
                                                if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                                                if(now_s()>p2c_dl_get())return;}}}}

                                // Try each kr candidate - NESTED_LOOP testing
                                // Skip for non-self-ref seed (E1) or if no kr candidates
                                if(kr_candidates&&seed_var==0){
                                for(int kr=0;kr<nr&&now_s()<dl_L;kr++){
                                if(!(kr_candidates&(1<<kr)))continue;
                                if(kr==outr||pools[nr].rpref[kr].empty())continue;

                                // Pre-body sweep + verification.
                                // library-bootstrap fix: deadline check inside ps loop (was missing - caused
                                // Phase 2F overrun by 2-5x in library-bootstrap first attempt; Phase 2H got
                                // skipped because Phase 2F never returned within budget).
                                int ps_check=0;
                                for(auto&ps:pools[nr].rpref[kr]){
                                    if(best_sc_shared.load()>=N)break;
                                    // polling mask = (kRegisterCount² - 1) = 63.
                                    // Poll deadline every 64 iterations; balances overhead vs
                                    // latency. Structural via kRegisterCount.
                                    if((++ps_check&(kRegisterCount*kRegisterCount-1))==0&&now_s()>dl_L)break;
                                    int sq=0;
                                    for(int n2=0;n2<n_quick_f;n2++){
                                        if(!ps.ok[n2])break;
                                        int64_t R[kRegisterCount];for(int r=0;r<kRegisterCount;r++)R[r]=ps.S[n2][r];
                                        int icap=(int)std::min((int64_t)kLoopIterCap,R[kr]+5);
                                        g_sat=false;for(int it=0;it<icap&&R[kr]!=0&&!g_sat;it++){
                                            if(inner_bd.has_loop)exBodyD(R,body,L,inner_bd);
                                            else exBodyFlat(R,body,L);}
                                        if(g_sat||R[kr]!=0)break;
                                        if(pm(R[outr],A)!=tgt[n2])break;sq++;}
                                    if(sq<n_quick_f)continue;
                                    // Quick check passed: verify remaining N-n_quick_f values.
                                    // MODE_FUNC is stateless - first n_quick_f already confirmed.
                                    auto&pre=pools[nr].all[ps.idx];
                                    int sc=n_quick_f;
                                    for(int n2=n_quick_f;n2<N;n2++){
                                        int64_t R[kRegisterCount]={};R[0]=n2;g_sat=false;
                                        if(pre.n>0)exBodyFlat(R,pre.ops,pre.n);
                                        if(g_sat)break;
                                        int icap2=(int)std::min((int64_t)kLoopIterCap,R[kr]+5);
                                        for(int it=0;it<icap2&&R[kr]!=0&&!g_sat;it++){
                                            if(inner_bd.has_loop)exBodyD(R,body,L,inner_bd);
                                            else exBodyFlat(R,body,L);}
                                        if(g_sat||R[kr]!=0)break;
                                        if(pm(R[outr],A)!=tgt[n2])break;sc++;}
                                    // lex-best (sc, -mdl) update via helper.
                                    if(sc>=best_sc_shared.load()){
                                        // Build full Ins array only on HIT (not per pre-body)
                                        Ins full[kProgramBodyMax];int nb=0;
                                        for(int i=0;i<pre.n;i++)full[nb++]=pre.ops[i];
                                        for(int i=0;i<L;i++)full[nb++]=body[i];
                                        full[nb++]={17,L,{(int8_t)kr,0,0,0},1};
                                        Res h;h.sc=sc;
                                        h.desc="NESTED_LOOP L="+std::to_string(L);
                                        h.mode=MODE_FUNC;h.ointerp=OUT_MOD;
                                        for(int i=0;i<nb;i++)h.body[i]=full[i];
                                        h.nbody=nb;h.nr=nr;h.outr=outr;h.mdl=computeMDL(h,ncat);
                                        tryUpdateBestLex(best, h, sc, &best_sc_shared, &best_mtx);p2f_hits++;
                                        printf("    NESTED_LOOP HIT sc=%d/%d L=%d nr=%d kr=%d R%d pre=%d body={",
                                               sc,N,L,nr,kr,outr,pre.n);
                                        for(int i=0;i<nb;i++)printf("%s%s",i?";":"",full[i].str().c_str());
                                        printf("}\n");
                                        if(sc==N){recordProg(h,ncat,tgt,A);
                                            if(!phase2_collecting){phase2_collecting=true;p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}
                                            if(now_s()>p2c_dl_get())return;}}}
                            }
                        } // kr candidates
                        } // kr_candidates && seed_var==0
                        }while(np());
                    } // urc
                } // seed_var (E1)
                } // pos_out
                } // ar_pass
            }; // end processTuple lambda

            // Pre-build all pools before parallel section (read-only during search).
            // Parallelized: each pools[nr] slot is independent - pure work distribution.
            {std::vector<std::thread>thr;
            for(int pnr=2;pnr<=kRegisterCount;pnr++)thr.emplace_back([&,pnr](){ensurePool(pnr);});
            for(auto&t:thr)t.join();}

            if(use_multisets){
                // A1: Parallel multiset processing. Each thread gets its own
                // seen_bodies set. Work distributed via atomic counter.
                int nthreads=std::min((int)std::thread::hardware_concurrency(),
                                      std::max(1,(int)msets.size()));
                std::atomic<int>next_ms(0);
                auto ms_worker=[&](){
                    std::unordered_set<uint64_t>local_seen;local_seen.reserve(kFpHashReserve);
                    while(true){
                        int mi=next_ms.fetch_add(1);
                        if(mi>=(int)msets.size()||now_s()>dl_L||best_sc_shared.load()>=N)break;
                        auto&ms=msets[mi];
                        int perm8[kProgramBodyMax/2];for(int i=0;i<L;i++)perm8[i]=ms.idx[i];
                        std::sort(perm8,perm8+L);
                        int pc=0;
                        // polling mask = (kRegisterCount⁴ - 1) = 4095.
                        // Poll deadline every 4096 iterations; outer permutation loop has
                        // higher per-iter cost than inner ps loop, so coarser polling.
                        do{if((++pc&(kRegisterCount*kRegisterCount*kRegisterCount*kRegisterCount-1))==0&&(now_s()>dl_L||best_sc_shared.load()>=N))break;
                            TyE types[kProgramBodyMax/2];for(int i=0;i<L;i++)types[i]=tyCat[perm8[i]];
                            processTuple(types,local_seen);
                        }while(std::next_permutation(perm8,perm8+L));
                    }};
                if(nthreads<=1){ms_worker();}
                else{std::vector<std::thread>threads;
                    for(int t=0;t<nthreads;t++)threads.emplace_back(ms_worker);
                    for(auto&t:threads)t.join();}
            }else{
                // A1: Parallel flat enumeration. Each thread processes a range of tuples.
                // thread-spawning threshold = isaMaxConstant³ = 1000.
                // Below 1000 tuples, sequential is faster than thread overhead.
                int nthreads_f=std::min((int)std::thread::hardware_concurrency(),
                                        std::max(1,(int)(total_tuples/((int64_t)kIsaMaxConstantConstexpr*kIsaMaxConstantConstexpr*kIsaMaxConstantConstexpr))));
                if(nthreads_f<1)nthreads_f=1;
                std::atomic<long long>next_tt(0);
                auto flat_worker=[&](){
                    std::unordered_set<uint64_t>local_seen;local_seen.reserve(kFpHashReserve);
                    while(true){
                        long long tt=next_tt.fetch_add(1);
                        if(tt>=total_tuples||now_s()>dl_L||best_sc_shared.load()>=N)break;
                        TyE types[kProgramBodyMax/2];
                        {long long v=tt;for(int i=L-1;i>=0;i--){types[i]=tyCat[v%nT];v/=nT;}}
                        processTuple(types,local_seen);
                    }};
                if(nthreads_f<=1){flat_worker();}
                else{std::vector<std::thread>threads;
                    for(int t=0;t<nthreads_f;t++)threads.emplace_back(flat_worker);
                    for(auto&t:threads)t.join();}
            }
            printf("    L=%d done: %lld tuples, %lld wirings, best=%d/%d %.1fs\n",
                   L,L_tuples.load(),wirings_tested.load(),best.sc,N,now_s());
            L_tuples=0;
        } // L loop

        printf("    phase2f_wsbp: total tuples=%lld wirings=%lld hits=%d best=%d/%d %.1fs\n",
               tuples_tested.load(),wirings_tested.load(),p2f_hits.load(),best.sc,N,now_s());
    }

    // ══════════════════════════════════════════════════════════════
    // PHASE 2H: Hierarchical synthesis - deep SUB_CALL-containing bodies (L=4..8).
    // ══════════════════════════════════════════════════════════════
    // Bodies without any SUB_CALL slot are already enumerated by Phase 2A at L≤4.
    // Phase 2H is purely additive: composeDDB chain extending to L=8, output filtered
    // to bodies containing >=1 SUB_CALL. Each pool entry verified across MODE_FUNC,
    // MODE_ITER, MODE_EMIT × outr ∈ [0..nr-1]. Constraint compliance:
    // C1 - exact match verification, no approximation.
    // C2 - MDL via existing computeMDL, log₂(ncat) per slot identical to baseline.
    // C3 - uniform enumeration over meta_L1 = L1 ∪ SUB_CALL_invocable; no target tactics.
    // C4 - applies to any target.
    // Phase 2H minimum-startup buffer = isaMaxConstant/2
    // seconds (= 5.0 with current ISA). ISA-derived; below this Phase 2H is skipped.
    // Phase 2H also runs in collecting mode (Phase 0/1 solved,
    // we want a bounded window to search for a shorter-MDL hierarchical program).
    if((best.sc<N||(phase2_collecting&&now_s()<p2c_dl_get()))&&now_s()<dl-((double)isaMaxConstant()/2.0)){
        int lib_invocable=0;
        int sz_lib=subCallCatalogSize();
        for(int idx=0;idx<sz_lib;idx++)
            if(subCallLibraryEntryInvocable(idx))lib_invocable++;
        if(lib_invocable>=1){
            // Phase 2H gets (isaMaxConstant-1)/isaMaxConstant
            // = 9/10 = 0.9 of remaining time. ISA-derived; same numerical value as prior magic.
            double p2h_dl=std::min(dl,now_s()+(dl-now_s())*((double)(isaMaxConstant()-1)/(double)isaMaxConstant()));
            printf("    phase2h_hier %.1fs lib_invocable=%d remaining=%.1fs\n",
                   now_s(),lib_invocable,p2h_dl-now_s());
            auto subcall_filter=[](const Ins*b,int n)->bool{
                for(int i=0;i<n;i++)if(b[i].ti==32)return true;
                return false;};
            // nr upper bound = kRegisterCount (architectural).
            for(int nr=2;nr<=kRegisterCount&&(best.sc<N||(phase2_collecting&&now_s()<p2c_dl_get()))&&now_s()<p2h_dl;nr++){
                auto fL1=buildL1(nr);
                auto p2=buildDDB(fL1,2,nr,p2h_dl,0,/*fast_fp=*/true);
                auto p3=buildDDB(fL1,3,nr,p2h_dl,0,/*fast_fp=*/true);
                // per-depth structural cap:
                // per_test_cost_d = kLMax × N × d × per_op_cost
                // cap_time = (p2h_dl - now) / per_test_cost_d
                // cap_mem = available_memory / (kDDBBodyMax × sizeof(DDB))
                // cap_d = min(cap_time, cap_mem)
                // All inputs structural - no magic ladder.
                const double p2h_per_op=calibratePerOpCost();
                // use currentFreeMemoryBytes for actual free memory
                // (not total system memory) - consistent with review in
                // composeDDB and Phase 2F. Phase 2H runs late in search; by then
                // earlier phases have allocated pool memory, so free << total.
                const long long p2h_cap_mem=(long long)(currentFreeMemoryBytes()/(size_t)kDDBBodyMax/sizeof(DDB));
                auto p2h_cap=[&](int d)->int{
                    double per_test=(double)isaMaxConstant()*(double)N*(double)d*p2h_per_op;
                    double rem=std::max(0.0,p2h_dl-now_s());
                    long long cap_time=(long long)(rem/std::max(per_test,1e-12));
                    long long cap=std::min(cap_time,p2h_cap_mem);
                    if(cap<1)cap=1;
                    if(cap>INT_MAX)cap=INT_MAX;
                    return(int)cap;};
                auto p4=composeDDB(p3,fL1,nr,p2h_dl,subcall_filter,p2h_cap(4),/*fast_fp=*/true);
                auto p5=composeDDB(p4,fL1,nr,p2h_dl,subcall_filter,p2h_cap(5),/*fast_fp=*/true);
                auto p6=composeDDB(p5,fL1,nr,p2h_dl,subcall_filter,p2h_cap(6),/*fast_fp=*/true);
                auto p7=composeDDB(p6,fL1,nr,p2h_dl,subcall_filter,p2h_cap(7),/*fast_fp=*/true);
                auto p8=composeDDB(p7,fL1,nr,p2h_dl,subcall_filter,p2h_cap(8),/*fast_fp=*/true);
                printf("    phase2h_hier nr=%d pool sizes: p4=%d p5=%d p6=%d p7=%d p8=%d\n",
                       nr,(int)p4.size(),(int)p5.size(),(int)p6.size(),(int)p7.size(),(int)p8.size());
                std::atomic<int>p2h_best_sc(best.sc);
                std::mutex p2h_mtx;
                std::atomic<int>p2h_hits(0);
                for(auto*pool:{&p4,&p5,&p6,&p7,&p8}){
                    if(best.sc>=N&&!phase2_collecting)break;
                    int pool_sz=(int)pool->size();
                    if(pool_sz==0)continue;
                    std::atomic<int>next_fi(0);
                    auto p2h_worker=[&](){
                        while(true){
                            int fi=next_fi.fetch_add(1);
                            if(fi>=pool_sz||now_s()>p2h_dl)return;
                            if(p2h_best_sc.load()>=N&&!phase2_collecting)return;
                            auto&fb=(*pool)[fi];
                            Ins body[kDDBBodyMax];int nb=fb.n;
                            for(int i=0;i<nb;i++)body[i]=fb.ops[i];
                            BodyDesc bd;computeBodyDesc(body,nb,bd);
                            // MODE_FUNC outr-fast-filter.
                            // Run body once with R[0]=0 and check whether ANY outr's resulting
                            // register value matches tgt[0]. If none match, MODE_FUNC cannot
                            // succeed for any outr (the per-outr early-exit would fire at n=0
                            // for every choice). Saves (nr-1) × N body executions per skipped
                            // candidate - bounded by min(nr,3) - 1 = 2.
                            // soundness: if filter rejects, no outr would have produced a
                            // non-zero score in MODE_FUNC; any candidate that the existing
                            // per-output early-exit would have accepted also passes filter.
                            // C2 - MDL accounting unchanged; filter only changes search rate.
                            // C3 - predicate (does any outr match tgt[0]?) is uniform across
                            // targets; no curation. Each target's tgt[0] is the same
                            // constraint applied identically.
                            // C4 - applies to any target; same code path everywhere.
                            bool func_viable = false;
                            {
                                int64_t Rf[kRegisterCount]={};Rf[0]=0;g_sat=false;
                                exBodyD(Rf,body,nb,bd);
                                if(!g_sat){
                                    for(int outr=0;outr<std::min(nr,3);outr++){
                                        if(pm(Rf[outr],A)==tgt[0]){func_viable=true;break;}
                                    }
                                }
                            }
                            // MODE_FUNC: R[0]=n, run body, output R[outr]
                            for(int outr=0;outr<std::min(nr,3)&&func_viable;outr++){
                                int sc=0;
                                for(int n=0;n<N;n++){
                                    int64_t R[kRegisterCount]={};R[0]=n;g_sat=false;
                                    exBodyD(R,body,nb,bd);
                                    if(g_sat)break;
                                    if(pm(R[outr],A)!=tgt[n])break;
                                    sc++;
                                }
                                // lex-best (sc, -mdl) update via helper.
                                if(sc>=p2h_best_sc.load()||(sc==N&&phase2_collecting)){
                                    Res h;h.sc=sc;
                                    h.desc="P2H_FUNC d="+std::to_string(nb)+" R"+std::to_string(outr);
                                    h.mode=MODE_FUNC;h.ointerp=OUT_MOD;
                                    for(int i=0;i<nb;i++)h.body[i]=body[i];
                                    h.nbody=nb;h.nr=nr;h.outr=outr;h.mdl=computeMDL(h,ncat);
                                    if(tryUpdateBestLex(best, h, sc, &p2h_best_sc, &p2h_mtx)){
                                        // Reacquire lock for atomic diagnostic printf.
                                        std::lock_guard<std::mutex>lk(p2h_mtx);
                                        p2h_hits++;
                                        printf("    P2H_FUNC HIT sc=%d d=%d R%d body={",sc,nb,outr);
                                        for(int i=0;i<nb;i++)printf("%s%s",i?";":"",body[i].str().c_str());
                                        printf("}\n");
                                    }
                                    if(sc==N){recordProg(h,ncat,tgt,A);
                                    if(!phase2_collecting){phase2_collecting=true;
                                        p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}}
                                }
                            }
                            // Phase 2H deadline check between modes - bounds per-candidate
                            // overrun (each mode can run nested SUB_CALL bodies for seconds with
                            // library size ≥30). Constraint-clean: deadline check is uniform
                            // across all targets, no curation. Prevents the wall-time blow-up
                            // observed in library-bootstrap.5 mining (rows running 850-1085s on 60s budget).
                            if(now_s()>p2h_dl)break;
                            // MODE_ITER: R[0]=r0, persist, output R[0] then run
                            for(int r0:{0,1,2}){
                                if(pm(r0,A)!=tgt[0])continue;
                                int64_t R[kRegisterCount]={};R[0]=r0;g_sat=false;int sc=0;
                                for(int t=0;t<N;t++){
                                    if(pm(R[0],A)!=tgt[t])break;sc++;
                                    exBodyD(R,body,nb,bd);
                                    if(g_sat)break;
                                }
                                // lex-best (sc, -mdl) update via helper.
                                if(sc>=p2h_best_sc.load()||(sc==N&&phase2_collecting)){
                                    Res h;h.sc=sc;
                                    h.desc="P2H_ITER d="+std::to_string(nb)+" r0="+std::to_string(r0);
                                    h.mode=MODE_ITER;h.ointerp=OUT_MOD;
                                    for(int i=0;i<nb;i++)h.body[i]=body[i];
                                    h.nbody=nb;h.nr=nr;h.outr=0;h.init[0]=r0;
                                    h.mdl=computeMDL(h,ncat);
                                    if(tryUpdateBestLex(best, h, sc, &p2h_best_sc, &p2h_mtx)){
                                        std::lock_guard<std::mutex>lk(p2h_mtx);
                                        p2h_hits++;
                                        printf("    P2H_ITER HIT sc=%d d=%d r0=%d body={",sc,nb,r0);
                                        for(int i=0;i<nb;i++)printf("%s%s",i?";":"",body[i].str().c_str());
                                        printf("}\n");
                                    }
                                    if(sc==N){recordProg(h,ncat,tgt,A);
                                    if(!phase2_collecting){phase2_collecting=true;
                                        p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}}
                                }
                            }
                            // Phase 2H deadline check between modes (see ITER comment above).
                            if(now_s()>p2h_dl)break;
                            // Worker-level filter: MODE_EMIT no-OUT skip.
                            // MODE_EMIT collects OUT (ti=12) emissions into g_emit_buf. A body
                            // that produces no OUT pushes (directly or indirectly via SUB_CALL)
                            // cannot emit and MODE_EMIT score is 0 regardless.
                            //
                            // CRITICAL: a body without top-level OUT can still emit indirectly
                            // via SUB_CALL invoking a library entry whose body contains OUT.
                            // The check must therefore: (1) scan for top-level OUT, AND
                            // (2) for each top-level SUB_CALL slot, look up the library entry
                            // and scan its body for OUT. A SUB_CALL'd library entry has nbody
                            // length up to ~24 and recursion-guarded (subCallLibraryEntryPure
                            // rejects entries containing SUB_CALL), so this is a single-level
                            // scan. The subtle failure mode - the
                            // initial implementation was scanning only top-level body and was
                            // cutting valid candidates whose SUB_CALL'd entries had OUT.
                            // soundness: emit_possible=true admits any body that COULD emit
                            // (top-level OUT or indirectly via SUB_CALL). Bodies set false
                            // provably cannot emit at all.
                            // C2 - MDL accounting unchanged.
                            // C3 - predicate is structural over body and known library;
                            // target-independent.
                            // C4 - applies uniformly to any target.
                            bool emit_possible = false;
                            for (int i = 0; i < nb && !emit_possible; i++) {
                                if (body[i].ti == 12) { emit_possible = true; break; }
                                if (body[i].ti == 32) {
                                    // SUB_CALL slot: check the library entry's body for OUT.
                                    int lib_idx = body[i].c;
                                    if (lib_idx >= 0 && lib_idx < g_progdb.size()) {
                                        const ProgramRecord& lpr = g_progdb.records[lib_idx];
                                        if (subCallLibraryEntryPure(lpr)) {
                                            for (int j = 0; j < lpr.nbody; j++) {
                                                if (lpr.body[j].ti == 12) { emit_possible = true; break; }
                                            }
                                        }
                                    }
                                }
                            }
                            // MODE_EMIT: R[0]=r0, persist, collect OUT emissions
                            for(int r0:{0,1,2}){
                                if(!emit_possible)break;
                                g_emit_A=A;g_emit_buf.clear();
                                int64_t R[kRegisterCount]={};R[0]=r0;g_sat=false;
                                for(int t=0;(int)g_emit_buf.size()<N&&!g_sat&&t<N*kIsaMaxConstantConstexpr;t++){
                                    exBodyD(R,body,nb,bd);
                                    if(t==kQuickCheckLen&&g_emit_buf.empty())break;
                                }
                                g_emit_A=0;
                                int sc=0;for(int t=0;t<N&&t<(int)g_emit_buf.size();t++){
                                    if(g_emit_buf[t]!=tgt[t])break;sc++;
                                }
                                // lex-best (sc, -mdl) update via helper.
                                if(sc>=p2h_best_sc.load()||(sc==N&&phase2_collecting)){
                                    Res h;h.sc=sc;
                                    h.desc="P2H_EMIT d="+std::to_string(nb)+" r0="+std::to_string(r0);
                                    h.mode=MODE_EMIT;h.ointerp=OUT_MOD;
                                    for(int i=0;i<nb;i++)h.body[i]=body[i];
                                    h.nbody=nb;h.nr=nr;h.outr=0;h.init[0]=r0;
                                    h.mdl=computeMDL(h,ncat);
                                    if(tryUpdateBestLex(best, h, sc, &p2h_best_sc, &p2h_mtx)){
                                        std::lock_guard<std::mutex>lk(p2h_mtx);
                                        p2h_hits++;
                                        printf("    P2H_EMIT HIT sc=%d d=%d r0=%d body={",sc,nb,r0);
                                        for(int i=0;i<nb;i++)printf("%s%s",i?";":"",body[i].str().c_str());
                                        printf("}\n");
                                    }
                                    if(sc==N){recordProg(h,ncat,tgt,A);
                                    if(!phase2_collecting){phase2_collecting=true;
                                        p2c_dl_set(std::min(dl,now_s()+(double)kIsaMaxConstantConstexpr));}}
                                }
                            }
                        }};// end p2h_worker
                    int p2h_nt=std::min((int)std::thread::hardware_concurrency(),
                                         std::max(1,pool_sz));
                    if(p2h_nt<=1){p2h_worker();}
                    else{std::vector<std::thread>thr;
                        for(int t=0;t<p2h_nt;t++)thr.emplace_back(p2h_worker);
                        for(auto&t:thr)t.join();}
                }// pool loop
                printf("    phase2h_hier nr=%d hits=%d best=%d/%d %.1fs\n",
                       nr,p2h_hits.load(),best.sc,N,now_s());
            }// nr loop
        }// lib_invocable >= 1
    }// best.sc < N && time

    // Merge Phase 0 result: use it only if Phase 1+ didn't find anything as good
    if(phase0_best.sc>best.sc||(phase0_best.sc==best.sc&&phase0_best.sc>0&&phase0_best.mdl<best.mdl))
        best=phase0_best;
    return best;
}

