// Updated bench/util.h to support parsing group_id from configuration file

#ifndef UTIL_H
#define UTIL_H

#include <string>
#include <fstream>
#include <iostream>

// Function to read group_id from config file
int read_group_id(const std::string &config_file) {
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "Could not open the configuration file: " << config_file << std::endl;
        return -1; // Return an error code
    }
    int group_id;
    // Assuming the config file has a line like: group_id=123
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("group_id=") == 0) {
            group_id = std::stoi(line.substr(9));
            file.close();
            return group_id;
        }
    }
    file.close();
    std::cerr << "group_id not found in the configuration file." << std::endl;
    return -1; // Return an error code
}

#endif // UTIL_H