/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FREQUENCY_MANAGER_HPP
#define FREQUENCY_MANAGER_HPP

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sandstone.h>

#define BASE_FREQ_PATH         "/sys/devices/system/cpu/cpu"
#define SCALING_GOVERNOR       "/cpufreq/scaling_governor"
#define SCALING_SETSPEED       "/cpufreq/scaling_setspeed"

class FrequencyManager
{
private:
    int max_frequency_supported;
    int min_frequency_supported;
    std::vector<std::string> per_cpu_initial_scaling_governor;
    std::vector<std::string> per_cpu_initial_scaling_setspeed;
    int current_set_frequency;
    std::vector<int> frequency_levels;
    int frequency_level_idx = 0;
    static constexpr int total_frequency_levels = 9;

    std::string read_file(std::filesystem::path &f_path)
    {
        /* Read first line of given file */
        std::string line;
        std::ifstream in_file(f_path.c_str());

        if (in_file.good())
            getline(in_file, line);
        else {
            fprintf(stderr, "cannot read from file \"%s\"\n", f_path.string().c_str());
            exit(EXIT_FAILURE);
        }

        in_file.close();
        return line;
    }

    void write_file(std::filesystem::path &f_path, std::string line)
    {
        /* Write first line of given file */
        std::ofstream out_file(f_path, std::ofstream::out);
        if (out_file.good())
            out_file.write(line.c_str(), line.length());
        else {
            fprintf(stderr, "cannot write \"%s\" to file \"%s\"\n", line.c_str(), f_path.string().c_str());
            exit(EXIT_FAILURE);
        }

        out_file.close();
    }

    int get_frequency_from_file(std::filesystem::path &f_path)
    {
        /* Read and convert value from file */
        int value;
        std::string line = read_file(f_path);

        value = std::stod(line);
        return value;
    }

    void populate_frequency_levels()
    {
        frequency_levels.push_back(max_frequency_supported);
        frequency_levels.push_back(min_frequency_supported); 

        std::vector<int> tmp = frequency_levels;

        while (frequency_levels.size() != total_frequency_levels)
        {
            std::sort(tmp.begin(), tmp.end(), std::greater<int>());
            for (int idx = 1; idx < tmp.size(); idx++)
                frequency_levels.push_back((tmp[idx] + tmp[idx - 1]) / 2);
            tmp = frequency_levels;
        }
    }

public:
    FrequencyManager() {}

    void initial_setup()
    {
        /* record supported max and min frequencies */
        std::filesystem::path cpuinfo_max_freq_path = "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq";
        max_frequency_supported = get_frequency_from_file(cpuinfo_max_freq_path);

        std::filesystem::path cpuinfo_min_freq_path = "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq";
        min_frequency_supported = get_frequency_from_file(cpuinfo_min_freq_path);

        // record different frequencies
        populate_frequency_levels();

        // save states
        for (int cpu = 0; cpu < num_cpus(); cpu++) {
            //save scaling governor for every cpu
            std::filesystem::path scaling_governor_path = BASE_FREQ_PATH;
            scaling_governor_path += std::to_string(cpu);
            scaling_governor_path += SCALING_GOVERNOR;
            per_cpu_initial_scaling_governor.push_back(read_file(scaling_governor_path));

            //save frequency for every cpu
            std::filesystem::path initial_scaling_setspeed_frequency_path = BASE_FREQ_PATH;
            initial_scaling_setspeed_frequency_path += std::to_string(cpu);
            initial_scaling_setspeed_frequency_path += SCALING_SETSPEED;
            per_cpu_initial_scaling_setspeed.push_back(read_file(initial_scaling_setspeed_frequency_path));

            //change scaling_governor to userspace to have different frequencies
            write_file(scaling_governor_path, "userspace");
        } 
    }

    void change_frequency()
    {
        current_set_frequency = frequency_levels[frequency_level_idx++ % total_frequency_levels];
        
        for (int cpu = 0; cpu < num_cpus(); cpu++) {
            std::filesystem::path scaling_setspeed = BASE_FREQ_PATH;
            scaling_setspeed += std::to_string(cpu);
            scaling_setspeed += SCALING_SETSPEED;
            write_file(scaling_setspeed, std::to_string(current_set_frequency));
        }
    }

    void restore_initial_state()
    {
        for (int cpu = 0; cpu < num_cpus(); cpu++) {
            //restore saved scaling governor for every cpu
            std::filesystem::path scaling_governor_path = BASE_FREQ_PATH;
            scaling_governor_path += std::to_string(cpu);
            scaling_governor_path += SCALING_GOVERNOR;
            write_file(scaling_governor_path, per_cpu_initial_scaling_governor[cpu]);

            //restore saved frequency for every cpu
            std::filesystem::path scaling_setspeed_path = BASE_FREQ_PATH;
            scaling_setspeed_path += std::to_string(cpu);
            scaling_setspeed_path += SCALING_SETSPEED;
            write_file(scaling_setspeed_path, per_cpu_initial_scaling_setspeed[cpu]);
        }
    }

    void reset_frequency_level_idx()
    {
        frequency_level_idx = 0;
    }
};
#endif //FREQUENCY_MANAGER_HPP