#include "sylar/sylar.h"
#include <iostream>

int main(int argc, char * argv[]) {
    std::cout << "test start" << std::endl;
    auto uri = sylar::Uri::Create("http://a:b@host.com:8080/p/a/t/h?query=string#hash");
    std::cout << uri->toString() << std::endl;
    return 0;
}