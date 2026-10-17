// Copyright (C) 2026 Aeolus fork contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "aeolus/simd_neon.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#if AEOLUS_HAS_NEON
namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void equal(const std::vector<float>& a, const std::vector<float>& b)
{
    check(a.size()==b.size() && std::memcmp(a.data(),b.data(),a.size()*sizeof(float))==0,
          "NEON output, scalar tail or guard differs from separate-operation reference");
}
void tests()
{
    using namespace aeolus;
    for (size_t n : {0,1,2,3,4,5,6,7,8,9,14,16,31,64,127,256,1024,4096}) {
        for (size_t offset : {0,1,2,3}) {
            std::vector<float> a(n+16), b(n+16);
            for (size_t i=0;i<a.size();++i) {
                a[i]=float(int((i*731+53)%1009)-504)/512;
                b[i]=float(int((i*313+17)%1013)-506)/512;
            }
            for (float k : {0.0f,1.0f,-0.37f}) {
                for (int operation=0;operation<4;++operation) {
                    for (bool alias : {false,true}) {
                        auto actual=a, expected=a;
                        const float* in=alias ? actual.data()+offset : b.data()+offset;
                        neon::add(actual.data()+offset,in,0); // zero count never touches guards
                        for (size_t i=offset;i<offset+n;++i) {
                            float v=alias ? expected[i] : b[i];
                            switch(operation) {
                                case 0: expected[i]+=v; break;
                                case 1: expected[i]+=v*k; break;
                                case 2: expected[i]=k*(expected[i]+v); break;
                                case 3: expected[i]*=k; break;
                            }
                        }
                        switch(operation) {
                            case 0: neon::add(actual.data()+offset,in,n); break;
                            case 1: neon::mul_const_add(actual.data()+offset,in,k,n); break;
                            case 2: neon::add_mul_const(actual.data()+offset,in,k,n); break;
                            case 3: neon::mul_const(actual.data()+offset,k,n); break;
                        }
                        equal(actual,expected);
                    }
                }
            }
            double dot=0, magnitude=0;
            for(size_t i=offset;i<offset+n;++i) { double p=double(a[i])*b[i]; dot+=p; magnitude+=std::abs(p); }
            const float result=neon::mul_reduce(a.data()+offset,b.data()+offset,n);
            check(std::isfinite(result) && std::abs(double(result)-dot)<=2e-6*std::max(1.0,magnitude),"NEON dot product error");
            if(n%2) continue;
            for(bool conjugate : {false,true}) {
                for(int alias=0;alias<3;++alias) {
                    auto x=a,y=b,out=a;
                    float* destination=alias==1?x.data():alias==2?y.data():out.data();
                    auto expected=alias==2?b:a;
                    for(size_t i=offset;i<offset+n;i+=2) {
                        expected[i]=a[i]*b[i]-a[i+1]*b[i+1];
                        expected[i+1]=conjugate?-a[i]*b[i+1]-a[i+1]*b[i]:a[i]*b[i+1]+a[i+1]*b[i];
                    }
                    (conjugate?neon::complex_mul_conj:neon::complex_mul)(destination+offset,x.data()+offset,y.data()+offset,n);
                    equal(alias==1?x:alias==2?y:out,expected);
                }
            }
            std::vector<float> actual(2*n+16), expected;
            for(size_t i=0;i<actual.size();++i) actual[i]=float(int(i%101)-50)/64;
            expected=actual;
            for(size_t i=offset;i<offset+n;i+=2) {
                float real=expected[n+i]*b[i]-expected[n+i+1]*b[i+1];
                float imag=expected[n+i]*b[i+1]+expected[n+i+1]*b[i];
                expected[n+i]=expected[i]-real;expected[n+i+1]=expected[i+1]-imag;
                expected[i]+=real;expected[i+1]+=imag;
            }
            neon::fft_step(actual.data()+offset,b.data()+offset,n);equal(actual,expected);
        }
    }
    std::cout<<"NEON arithmetic: exact elementwise/complex/FFT output; dot error bounded; tails, unaligned addresses, aliases and guards passed\n";
}
// Benchmark separate, no-inline scalar kernels so the optimizer cannot remove
// work or hoist calls. Scalar compiler auto-vectorization is deliberately allowed.
#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE __declspec(noinline)
#endif
NOINLINE float scalarDot(const float* a,const float* b,size_t n) { float s=0;for(size_t i=0;i<n;++i)s+=a[i]*b[i];return s; }
NOINLINE void scalarComplex(float* out,const float* a,const float* b,size_t n) {
    for(size_t i=0;i<n;i+=2){float r=a[i]*b[i]-a[i+1]*b[i+1];float im=-a[i]*b[i+1]-a[i+1]*b[i];out[i]=r;out[i+1]=im;}
}
NOINLINE void scalarFft(float* d,const float* w,size_t n) {
    for(size_t i=0;i<n;i+=2){float r=d[n+i]*w[i]-d[n+i+1]*w[i+1];float im=d[n+i]*w[i+1]+d[n+i+1]*w[i];d[n+i]=d[i]-r;d[n+i+1]=d[i+1]-im;d[i]+=r;d[i+1]+=im;}
}
void benchmark()
{
    volatile double checksum=0;
    for(int kernel=0;kernel<3;++kernel) for(size_t n : {64,256,4096}) {
        std::vector<float> a(n*2,0.001f),b(n,0.25f),out(n);
        std::vector<double> times[2];
        for(int round=-10;round<100;++round) for(int order=0;order<2;++order) {
            int variant=(round+10+order)%2;
            float (*volatile dot)(const float*,const float*,size_t)=variant?aeolus::neon::mul_reduce:scalarDot;
            void (*volatile complex)(float*,const float*,const float*,size_t)=variant?aeolus::neon::complex_mul_conj:scalarComplex;
            void (*volatile fft)(float*,const float*,size_t)=variant?aeolus::neon::fft_step:scalarFft;
            auto start=std::chrono::steady_clock::now();
            for(int repeat=0;repeat<200;++repeat) {
                if(kernel==0) checksum+=dot(a.data(),b.data(),n);
                else if(kernel==1) { complex(out.data(),a.data(),b.data(),n); checksum+=out[0]; }
                else { std::fill(a.begin(),a.end(),0.001f);fft(a.data(),b.data(),n);checksum+=a[0]; }
            }
            auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/200;
            if(round>=0)times[variant].push_back(us);
        }
        for(auto& t:times)std::sort(t.begin(),t.end());
        std::cout<<"kernel="<<kernel<<" floats="<<n<<" scalar_us="<<times[0][50]<<" neon_us="<<times[1][50]<<" speedup="<<times[0][50]/times[1][50]<<'\n';
    }
    std::cout<<"checksum="<<checksum<<'\n';
}
}
#endif
int main(int argc,char** argv)
{
#if AEOLUS_HAS_NEON
    try { tests();if(argc==2 && std::string(argv[1])=="--benchmark")benchmark(); }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
#else
    (void)argc;(void)argv;
    std::cout<<"NEON not enabled for this target; existing arithmetic dispatch retained\n";
#endif
}
