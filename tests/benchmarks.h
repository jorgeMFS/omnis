#pragma once
// Self-contained benchmark target generators. No external dependencies.

#include <vector>
#include <cstdint>

inline std::vector<int> genCounting(int n) {
    std::vector<int> out(n); for(int i=0;i<n;i++) out[i]=i%4; return out; }

inline std::vector<int> genPow3Mod7(int n) {
    std::vector<int> out; int x=1;
    for(int i=0;i<n;i++){out.push_back(x);x=(x*3)%7;} return out; }

inline std::vector<int> genTriMod8(int n) {
    std::vector<int> out; int64_t t=0;
    for(int i=0;i<n;i++){out.push_back((int)(((t%8)+8)%8));t+=(i+1);} return out; }

inline std::vector<int> genFibMod4(int n) {
    std::vector<int> out; int64_t a=0,b=1;
    for(int i=0;i<n;i++){out.push_back((int)(((b%4)+4)%4));int64_t c=a+b;a=b;b=c;} return out; }

inline std::vector<int> genThueMorse(int n) {
    std::vector<int> out;
    for(int i=0;i<n;i++) out.push_back(__builtin_popcount((unsigned)i)%2); return out; }

inline std::vector<int> genDigitSum4(int n) {
    std::vector<int> out;
    for(int i=0;i<n;i++){int s=0,x=i;while(x>0){s+=x%4;x/=4;}out.push_back(s%4);} return out; }

inline std::vector<int> genCollatz(int seed, int n) {
    std::vector<int> out; int64_t x=seed;
    for(int i=0;i<n;i++){out.push_back((int)(((x%4)+4)%4));
    if(x%2==0)x/=2;else x=3*x+1;} return out; }

inline std::vector<int> genRule30(int n) {
    int w=2*n+3,c=n+1; std::vector<int> row(w,0),nr(w,0); row[c]=1;
    std::vector<int> out;
    for(int s=0;s<n;s++){out.push_back(row[c]);
    for(int i=1;i<w-1;i++)nr[i]=row[i-1]^(row[i]|row[i+1]);
    std::swap(row,nr);std::fill(nr.begin(),nr.end(),0);} return out; }

inline std::vector<int> genParityAlt(int n) {
    std::vector<int> out; int64_t r0=7,r1=1;
    for(int i=0;i<n;i++){int p=(int)(r0%2);out.push_back(p);
    if(p==0){r0=r0/2+r1;}else{r0=r0*3+1;r1++;}} return out; }

inline std::vector<int> genChampernowne(int n) {
    std::vector<int> out;
    for(int num=0;(int)out.size()<n;num++){
    if(num==0){out.push_back(0);}else{std::vector<int>d;int t=num;
    while(t>0){d.push_back(t%4);t/=4;}
    for(int i=(int)d.size()-1;i>=0;i--)out.push_back(d[i]);}}
    out.resize(n); return out; }

inline std::vector<int> genCollatzStop(int n) {
    std::vector<int> out;
    for(int i=0;i<n;i++){int64_t x=i+2;int c=0;
    while(x>1){if(x%2==0)x/=2;else x=3*x+1;c++;}out.push_back(c%4);} return out; }

inline std::vector<int> genPiB4(int n) {
    const char*h="243F6A8885A308D313198A2E03707344"
    "A4093822299F31D0082EFA98EC4E6C89452821E638D01377BE5466CF34E90C6C"
    "C0AC29B7C97C50DD3F84D5B5B54709179216D5D98979FB1BD1310BA698DFB5AC";
    std::vector<int> out; out.push_back(3);
    for(int i=0;h[i]&&(int)out.size()<n;i++){char c=h[i];
    int v=(c>='0'&&c<='9')?(c-'0'):(c>='A'&&c<='F')?(c-'A'+10):(c-'a'+10);
    out.push_back(v>>2);out.push_back(v&3);}
    out.resize(n); return out; }

// ── Nested LOOP targets: number-theoretic functions requiring trial division ──

// d(n+1) mod A: divisor count (OEIS A000005)
// Program: for k=1..n+1, if (n+1)%k==0 then count++. Output count%A.
inline std::vector<int> genDivisorCount(int n, int A=4) {
    std::vector<int> out;
    for(int i=0;i<n;i++){int v=i+1,d=0;
    for(int k=1;k<=v;k++)if(v%k==0)d++;
    out.push_back(d%A);} return out; }

// sigma(n+1) mod A: sum of divisors (OEIS A000203)
// Program: for k=1..n+1, if (n+1)%k==0 then sum+=k. Output sum%A.
inline std::vector<int> genSigma(int n, int A=4) {
    std::vector<int> out;
    for(int i=0;i<n;i++){int v=i+1;int64_t s=0;
    for(int k=1;k<=v;k++)if(v%k==0)s+=k;
    out.push_back((int)(s%A));} return out; }

// phi(n+1) mod A: Euler's totient (OEIS A000010)
// Program: for k=1..n+1, if gcd(k,n+1)==1 then count++. Output count%A.
inline std::vector<int> genEulerPhi(int n, int A=4) {
    std::vector<int> out;
    for(int i=0;i<n;i++){int v=i+1,cnt=0;
    for(int k=1;k<=v;k++){int a2=k,b2=v;
        while(b2){int t=b2;b2=a2%b2;a2=t;}if(a2==1)cnt++;}
    out.push_back(cnt%A);} return out; }

// omega(n+1) mod A: number of distinct prime factors (OEIS A001221)
// Program: trial division counting distinct primes.
inline std::vector<int> genOmega(int n, int A=4) {
    std::vector<int> out;
    for(int i=0;i<n;i++){int v=i+1,cnt=0;
    for(int k=2;k<=v;k++){if(v%k==0){cnt++;while(v%k==0)v/=k;}}
    out.push_back(cnt%A);} return out; }

// Largest prime factor of n+1, mod A (OEIS A006530)
inline std::vector<int> genLPF(int n, int A=4) {
    std::vector<int> out;
    for(int i=0;i<n;i++){int v=i+1,lpf=1;
    for(int k=2;k<=v;k++){while(v%k==0){lpf=k;v/=k;}}
    out.push_back(lpf%A);} return out; }

// pi(n) mod A: prime counting function (OEIS A000720)
// Program: for k=2..n+1, test primality by trial division.
inline std::vector<int> genPrimeCounting(int n, int A=4) {
    std::vector<int> out; int cnt=0;
    for(int i=0;i<n;i++){int v=i+1;bool prime=(v>=2);
    for(int k=2;(int64_t)k*k<=v;k++)if(v%k==0){prime=false;break;}
    if(prime)cnt++;out.push_back(cnt%A);} return out; }
