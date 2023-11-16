/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef FREQUENCY_MANAGER_HPP
#define FREQUENCY_MANAGER_HPP

#define BASE_FREQ_PATH         "/sys/devices/system/cpu/cpu"
#define SCALING_GOVERNOR       "/cpufreq/scaling_governor"
#define SCALING_SETSPEED       "/cpufreq/scaling_setspeed"

class FrequencyManager
{
private:
#ifdef __linux
    int max_frequency_supported;
    int min_frequency_supported;
    std::vector<std::string> per_cpu_initial_scaling_governor;
    std::vector<std::string> per_cpu_initial_scaling_setspeed;
    int current_set_frequency;
    std::vector<int> frequency_levels;
    int frequency_level_idx = 0;
    static constexpr int total_frequency_levels = 9;
#endif

    std::string read_file(const std::string &file_path)
    {
        /* Read first line of given file */
        char line[100]; //100 characters should be more than enough
        FILE *file = fopen(file_path.c_str(), "r");

        if (file == NULL) {
            fprintf(stderr, "%s: cannot read from file: %s :%m\n", program_invocation_name, file_path.c_str());
            exit(EXIT_FAILURE);
        }
        fscanf(file, "%s", line);
        fclose(file);
        return std::string(line);
    }

    int write_file(const std::string &file_path, const std::string &line)
    {
        FILE *file = fopen(file_path.c_str(), "w");

        if (file == NULL)
            return 1;

        fprintf(file, "%s", line.c_str());
        fclose(file);
        return 0;
    }

    int get_frequency_from_file(const std::string &file_path)
    {
        /* Read frequency value from file */
        int frequency;
        FILE *file = fopen(file_path.c_str(), "r");

        if (file == NULL) {
            fprintf(stderr, "%s: cannot read from file: %s :%m\n", program_invocation_name, file_path.c_str());
            exit(EXIT_FAILURE);
        }
        fscanf(file, "%d", &frequency);
        fclose(file);
        return frequency;
    }

    void populate_frequency_levels()
    {
#ifdef __linux__
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
#endif
    }

    void check_if_userspace_present()
    {
#ifdef __linux__
        const char *scaling_governor_path = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors";
        char read_file[100];
        FILE *file = fopen(scaling_governor_path, "r");

        if (file == NULL) {
            fprintf(stderr, "%s: cannot read from file: %s :%m\n", program_invocation_name, scaling_governor_path);
            exit(EXIT_FAILURE);
        }

        while (fscanf(file, "%s", read_file)) {
            if (strcmp(read_file, "userspace") == 0) {
                fclose(file);
                return;
            }
        }

        fprintf(stderr, "%s: Cannot find \"userspace\" scaling governor from the file: %s\n", program_invocation_name, scaling_governor_path);
        exit(EXIT_FAILURE);
#endif
    }

public:
    FrequencyManager() {}

    void initial_setup()
    {
#ifdef __linux__
        /* check if "userspace" frequency governor is available. Not all distributions have it*/
        check_if_userspace_present();    

        /* record supported max and min frequencies */
        std::string cpuinfo_max_freq_path{"/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"};
        max_frequency_supported = get_frequency_from_file(cpuinfo_max_freq_path);

        std::string cpuinfo_min_freq_path{"/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq"};
        min_frequency_supported = get_frequency_from_file(cpuinfo_min_freq_path);

        // populate different frequencies for each test to run
        populate_frequency_levels();

        // save states
        for (int cpu = 0; cpu < num_cpus(); cpu++) {
            //save scaling governor for every cpu
            std::string scaling_governor_path = BASE_FREQ_PATH;
            scaling_governor_path += std::to_string(cpu_info[cpu].cpu_number);
            scaling_governor_path += SCALING_GOVERNOR;
            per_cpu_initial_scaling_governor.push_back(read_file(scaling_governor_path));

            //save frequency for every cpu
            std::string initial_scaling_setspeed_frequency_path = BASE_FREQ_PATH;
            initial_scaling_setspeed_frequency_path += std::to_string(cpu_info[cpu].cpu_number);
            initial_scaling_setspeed_frequency_path += SCALING_SETSPEED;
            per_cpu_initial_scaling_setspeed.push_back(read_file(initial_scaling_setspeed_frequency_path));

            //change scaling_governor to userspace to have different frequencies
            if (write_file(scaling_governor_path, "userspace") == 1) {
                fprintf(stderr, "%s: cannot write userspace to file \"%s\". User should be root :%m\n", program_invocation_name, scaling_governor_path.c_str());
                exit(EXIT_NOPERMISSION);
            }
        }
#endif 
    }

    void change_frequency()
    {
#ifdef __linux__
        current_set_frequency = frequency_levels[frequency_level_idx++ % total_frequency_levels];
        
        for (int cpu = 0; cpu < num_cpus(); cpu++) {
            std::string scaling_setspeed = BASE_FREQ_PATH;
            scaling_setspeed += std::to_string(cpu_info[cpu].cpu_number);
            scaling_setspeed += SCALING_SETSPEED;
            if (write_file(scaling_setspeed, std::to_string(current_set_frequency)) == 1) {
                fprintf(stderr, "%s: cannot write current_frequency (%d) to the file \"%s\" :%m\n", 
                        program_invocation_name, current_set_frequency ,scaling_setspeed.c_str());
                exit(EXIT_NOPERMISSION);
            } 
        }
#endif
    }

    void restore_initial_state()
    {
#ifdef __linux__
        for (int cpu = 0; cpu < num_cpus(); cpu++) {
            //restore saved scaling governor for every cpu
            std::string scaling_governor_path = BASE_FREQ_PATH;
            scaling_governor_path += std::to_string(cpu_info[cpu].cpu_number);
            scaling_governor_path += SCALING_GOVERNOR;
            if (write_file(scaling_governor_path, per_cpu_initial_scaling_governor[cpu]) == 1) {
                fprintf(stderr, "%s: cannot write saved governor (%s) to the file \"%s\" :%m\n", 
                    program_invocation_name, per_cpu_initial_scaling_governor[cpu].c_str(), scaling_governor_path.c_str());
                exit(EXIT_NOPERMISSION);
            }

            //restore saved frequency for every cpu
            std::string scaling_setspeed_path = BASE_FREQ_PATH;
            scaling_setspeed_path += std::to_string(cpu_info[cpu].cpu_number);
            scaling_setspeed_path += SCALING_SETSPEED;
            if (write_file(scaling_setspeed_path, per_cpu_initial_scaling_setspeed[cpu]) == 1) {
                fprintf(stderr, "%s: cannot write saved scaling setspeed (%s) to the file \"%s\" :%m\n", 
                    program_invocation_name, per_cpu_initial_scaling_setspeed[cpu].c_str(), scaling_setspeed_path.c_str());
                exit(EXIT_NOPERMISSION);
            }
        }
#endif
    }

    void reset_frequency_level_idx()
    {
#ifdef __linux__
        frequency_level_idx = 0;
#endif
    }
};
#endif //FREQUENCY_MANAGER_HPP