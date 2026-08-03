#include <iostream> 
#include <unistd.h>
#include "MqttMasterManager.hpp"

int main() {
    std::cout << "===================="<< std::endl;
    std::cout << " Master node test "  << std::endl;
    std::cout << "===================="<< std::endl;


    MqttMasterManager manager_module;

    if(!manager_module.init("localhost",1883)) {
        std::cerr << "Failed to initialize Masternode module! " << std::endl;
        return 1;
    }

    std::cout << "[Master node] master node is running background " << std::endl;

    while(true) {
        sleep(1);
    }

    return 0;
}



