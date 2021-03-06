// RUN: %clang_cc1 -fsyntax-only -verify %s
class Base {
protected:
  Base(int val);
};


class Derived : public Base {
public:
  Derived(int val);
};


Derived::Derived(int val)
  :  Base( val )
{
}
