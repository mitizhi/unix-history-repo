// RUN: %clang_cc1 -triple x86_64-apple-darwin10 -emit-llvm -o - %s | FileCheck %s

struct A { 
  A();
  A(const A&);
  A(A&);
  ~A();
};

struct B {
  B();
  B(B&);
};

struct C {
  C() {}
  C(C& other, A a = A());
  int i, j;
};

struct POD {
  int array[3][4];
};

struct D : A, B, virtual C { 
  D();
  int scalar;
  int scalar_array[2][3];
  B class_member;
  C class_member_array[2][3];
  POD pod_array[2][3];

  union {
    int x;
    float f[3];
  };
};

void f(D d) {
  D d2(d);
}

// CHECK: define linkonce_odr void @_ZN1DC1ERS_(%struct.D* %this, %struct.D*) unnamed_addr
// CHECK: call void @_ZN1AC1Ev
// CHECK: call void @_ZN1CC2ERS_1A
// CHECK: call void @_ZN1AD1Ev
// CHECK: call void @_ZN1AC2ERS_
// CHECK: call void @_ZN1BC2ERS_
// CHECK: {{call void @llvm.memcpy.p0i8.p0i8.i64.*i64 24}}
// CHECK: call void @_ZN1BC1ERS_
// CHECK: br
// CHECK: {{icmp ult.*, 2}}
// CHECK: {{icmp ult.*, 3}}
// CHECK: call void @_ZN1AC1Ev
// CHECK: call void @_ZN1CC1ERS_1A
// CHECK: call void @_ZN1AD1Ev
// CHECK: {{call void @llvm.memcpy.p0i8.p0i8.i64.*i64 288}}
// CHECK: {{call void @llvm.memcpy.p0i8.p0i8.i64.*i64 12}}
// CHECK: ret void


template<class T> struct X0 { void f0(T * ) { } };
template <class > struct X1 { X1( X1& , int = 0 ) { } };
struct X2 { X1<int> result; };
void test_X2()
{
  typedef X2 impl;
  typedef X0<impl> pimpl;
  impl* i;
  pimpl pdata;
  pdata.f0( new impl(*i));
}
