#include <iostream>
#include <string>

class A{
    public:
    A() = default;
    ~A() = default;

    void GetName() const {
        std::cout<< _name  << "\n";
    }
    protected:
    std::string _name = "class A";
};

class B: public A{
    public:
    B() {
        //A::_name = "class B"; //reinitiating field of A class 
        };
    ~B() = default;

    protected:
    std::string _name = "class B";
};


int main()
{
    A a;
    B b;
    B* ptr = new(B);

    //ptr = &b;  //memory leak
    a.GetName();
    b.GetName();
    ptr->GetName();
}
