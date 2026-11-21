#include <yfoc_motor.hpp>
#include <iostream>
#include <string>

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: yfoc_inspect <interface> <channel> <node>\n";
        return 2;
    }
    try {
        const auto node = std::stoi(argv[3]);
        yfoc::Bus bus(argv[1], argv[2]);
        const auto info = bus.info(node);
        const auto state = bus.status(node);
        const auto sample = bus.feedback(node);
        std::cout << "node " << node << "  firmware " << info.fw_major << '.' << info.fw_minor
                  << "  fault " << state.fault << "  position " << sample.position
                  << " rad  speed " << sample.velocity << " rad/s\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
