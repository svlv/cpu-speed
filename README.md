# CPU clock speed monitor for Linux
Display frequency and usage for each thread in a terminal

![alt text](http://www.rabina.xyz/images/cpu-speed.gif "Cpu speed")

# Dependencies
- `lm_sensors` library is used to get data from cpu sensors.
- `termcap` library is used to support fullscreen mode in terminal.

Note you may need to install these dependencies depending on your Linux distro. 

# Building & Running
Clone the source code into a working directory and run `make` :
```
git clone git@github.com:svlv/cpu-speed.git
cd cpu-speed
make
```

To run from the working directory:
```
./cpu-speed
```

To install:
```
sudo make install
```

# Usage
It supports the full screen mode:
```
cpu-speed --fullscreen
```
To close press `q` or `Ctrl-C`.

# How does it work?
It reads all necessary data from the `sysfs` virtual filesystem. Below are the files and directories which are used:
| File or dir path          | Content                  |
| --------------|:---------------------|
| `/proc/cpuinfo` | General information about CPU |
| `/proc/stat`    | Data about current usage of each cpu |
| `/sys/devices/system/cpu/possible` | The list of possible cpuids |
| `/sys/devices/system/cpu/online` | The list of online cpuids |
| `/sys/devices/system/cpu/cpufreq/policy/` | Data from the driver about the current CPU frequency and topology |
