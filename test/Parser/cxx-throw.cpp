// RUN: %clang_cc1 -fexceptions -fsyntax-only -verify %s

int i;

void foo() {
  (throw,throw);
  (1 ? throw 1 : throw 2);
  throw int(1);
  throw;
  throw 1;
  throw;
  1 ? throw : (void)42;
  __extension__ throw 1;    // expected-error {{expected expression}}
  (void)throw;              // expected-error {{expected expression}}
}
