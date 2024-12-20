# PX-FUSE module
Exports a control plane to create virtual block devices in the linux namespace. Piggy-backs on FUSE transport to act as a conduit between kernel and user space. 

## Requirements
Requires kernel >= 3.10

### Building the PX-FUSE rpm/dpkg in docker

```
# export OUTPATH=/opt/pwx; docker run -v ${OUTPATH}:${OUTPATH} -v /usr/src:/usr/src -ti -e OUTPATH=$OUTPATH portworx/px-fuse
```

### Building the PX-FUSE module on ubuntu/debian

```
# git clone https://github.com/portworx/px-fuse.git
# cd px-fuse
# autoreconf && ./configure
# export KERNELPATH="/usr/src/linux-headers-`uname -r`"
# make 
# insmod px.ko

```

### Building the PX-FUSE package on Ubuntu
```
# apt-get install dh-autoreconf
# git clone https://github.com/portworx/px-fuse.git
# cd px-fuse/rpm
# # check your kernel version with 
# uname -a
3.19.0-43-generic
# # Make sure you have the kernel headers... if not, get them.
# ls /usr/src/linux-headers-<kernel version>
# # Make sure you have the needed build packages
# apt-get install rpm alien
# KERNELPATH="/usr/src/linux-headers-3.19.0-43-generic" VERSION=3.19.0 REVISION=43 ./buildrpm.sh
# # This will generate an install package in px-fuse/rpm/px/RPMS/x86_64/
# dpkg --install px_<version>.deb
```

### Building the PX-FUSE RPM on CentOS
```
# git clone https://github.com/portworx/px-fuse.git
# cd px-fuse/rpm
# uname -r
3.10.0-123.9.3.el7.x86_64
# # Make sure you have the kernel headers... if not, get them.
# ls /usr/src/kernels/
3.10.0-123.9.3.el7.x86_64
# KERNELPATH=/usr/src/kernels/3.10.0-123.el7.x86_64 VERSION=3.10.0 REVISION=123.9.3.el7 ./buildrpm.sh
# rpm -Uvh /root/px-fuse/rpm/px/RPMS/x86_64/px-3.10.0-123.9.3.el7.x86_64.rpm
```

### Building and running Tests 
```
A set of tests are included to test control operations attach and detach as well as I/O operations read and write.
To build test:

# cd px_fuse
# ./configure
# make pxd_test
# make test_clean

Make sure that px.ko can be built and sucessfully installed.
To run test:

# cd px_fuse
# ./test/pxd_test --gtest_filter=PxdTest.write
# ./test/pxd_test --gtest_filter=PxdTest.read
```

### Verifying PX-FUSE will build on Ubuntu

`kernel-tests.sh` is a script which can verify if headers from alternate kernels will build with PX-FUSE.  It can 

- downloads batches of kernel header files one at a time from the Ubuntu site.
- selects of what headers to test is based on a regular expression
- installs headers and attempts to build px-fuse
- reports success or failure and outputs log of failed builds
- optionally, it can output to separate log file

To find out how many headers are in a particular regex string, use the -n (noop) feature to display them.
For example,

    ./kernel-tests.sh -n 4.[012]

will scan for the headers for versions 4.0-4.2.

To build through a specific kernel and display the build errors, 

- use the **-n** option to display the range of kernels you want to test
- run the script with the string

```
# ./kernel-tests.sh -n 4.4
=== RUN 1/27 v4.4-rc1+cod1-wily/  (0.000601s) [noop]
=== RUN 2/27 v4.4-rc1-wily/  (0.000584s) [noop]
=== RUN 3/27 v4.4-rc2+cod1-wily/  (0.000587s) [noop]
=== RUN 4/27 v4.4-rc2-wily-keep/  (0.000585s) [noop]
=== RUN 5/27 v4.4-rc2-wily/  (0.000588s) [noop]
=== RUN 6/27 v4.4-rc3-wily/  (0.000583s) [noop]
=== RUN 7/27 v4.4-rc4-wily/  (0.000583s) [noop]
=== RUN 8/27 v4.4-rc5-wily/  (0.000583s) [noop]
=== RUN 9/27 v4.4-rc6-wily/  (0.000576s) [noop]
=== RUN 10/27 v4.4-rc7-wily/  (0.000578s) [noop]
=== RUN 11/27 v4.4-rc8-wily/  (0.000586s) [noop]
=== RUN 12/27 v4.4-wily/  (0.000583s) [noop]
=== RUN 13/27 v4.4.1-wily/  (0.000582s) [noop]
=== RUN 14/27 v4.4.2-wily/  (0.000579s) [noop]
=== RUN 15/27 v4.4.3-wily/  (0.000581s) [noop]
=== RUN 16/27 v4.4.4-wily/  (0.000602s) [noop]
=== RUN 17/27 v4.4.5-wily/  (0.000591s) [noop]
=== RUN 18/27 v4.4.6-wily/  (0.000584s) [noop]
=== RUN 19/27 v4.4.7-wily/  (0.000592s) [noop]
=== RUN 20/27 v4.4.8-wily/  (0.000583s) [noop]
=== RUN 21/27 v4.4.9-xenial/  (0.000582s) [noop]
=== RUN 22/27 v4.4.10-xenial/  (0.000583s) [noop]
=== RUN 23/27 v4.4.11-xenial/  (0.000581s) [noop]
=== RUN 24/27 v4.4.12-xenial/  (0.000596s) [noop]
=== RUN 25/27 v4.4.13-xenial/  (0.000591s) [noop]
=== RUN 26/27 v4.4.14-xenial/  (0.000584s) [noop]
=== RUN 27/27 v4.4.15/  (0.000598s) [noop]
# ./kernel-tests.sh 4.4-wily
=== RUN 1/1 v4.4-wily/  (22.23s)
Selecting previously unselected package linux-headers-4.4.0-040400-generic.
(Reading database ... 90678 files and directories currently installed.)
Preparing to unpack linux-headers-4.4.0-040400-generic_4.4.0-040400.201601101930_amd64.deb ...
Unpacking linux-headers-4.4.0-040400-generic (4.4.0-040400.201601101930) ...
Selecting previously unselected package linux-headers-4.4.0-040400.
Preparing to unpack linux-headers-4.4.0-040400_4.4.0-040400.201601101930_all.deb ...
Unpacking linux-headers-4.4.0-040400 (4.4.0-040400.201601101930) ...
Setting up linux-headers-4.4.0-040400 (4.4.0-040400.201601101930) ...
Setting up linux-headers-4.4.0-040400-generic (4.4.0-040400.201601101930) ...
checking for g++... g++
checking whether the C++ compiler works... yes
checking for C++ compiler default output file name... a.out
checking for suffix of executables... 
checking whether we are cross compiling... no
checking for suffix of object files... o
checking whether we are using the GNU C++ compiler... yes
checking whether g++ accepts -g... yes
checking for gcc... gcc
checking whether we are using the GNU C compiler... yes
checking whether gcc accepts -g... yes
checking for gcc option to accept ISO C89... none needed
checking whether make sets $(MAKE)... yes
checking how to run the C preprocessor... gcc -E
checking for grep that handles long lines and -e... /bin/grep
checking for egrep... /bin/grep -E
checking for ANSI C header files... yes
checking for sys/types.h... yes
checking for sys/stat.h... yes
checking for stdlib.h... yes
checking for string.h... yes
checking for memory.h... yes
checking for strings.h... yes
checking for inttypes.h... yes
checking for stdint.h... yes
checking for unistd.h... yes
checking fcntl.h usability... yes
checking fcntl.h presence... yes
checking for fcntl.h... yes
checking for stdint.h... (cached) yes
checking for stdlib.h... (cached) yes
checking sys/ioctl.h usability... yes
checking sys/ioctl.h presence... yes
checking for sys/ioctl.h... yes
checking sys/param.h usability... yes
checking sys/param.h presence... yes
checking for sys/param.h... yes
checking for stdbool.h that conforms to C99... yes
checking for _Bool... yes
checking for inline... inline
checking for size_t... yes
checking for ssize_t... yes
checking for uint32_t... yes
checking for uint64_t... yes
checking for memset... yes
configure: creating ./config.status
config.status: creating Makefile
config.status: creating config.h
make -C /usr/src/linux-headers-4.4.0-040400-generic M=/home/ubuntu/px-fuse modules
make[1]: Entering directory '/usr/src/linux-headers-4.4.0-040400-generic'
  CC [M]  /home/ubuntu/px-fuse/pxd.o
/home/ubuntu/px-fuse/pxd.c: In function 'pxd_process_read_reply':
/home/ubuntu/px-fuse/pxd.c:201:24: error: 'struct fuse_conn' has no member named 'iq'
 #define REQCTR(fc) (fc)->iq.reqctr
                        ^
/home/ubuntu/px-fuse/pxd.c:215:18: note: in expansion of macro 'REQCTR'
  trace_pxd_reply(REQCTR(fc), req->in.h.unique, 0u);
                  ^
/home/ubuntu/px-fuse/pxd.c: In function 'pxd_process_write_reply':
/home/ubuntu/px-fuse/pxd.c:201:24: error: 'struct fuse_conn' has no member named 'iq'
 #define REQCTR(fc) (fc)->iq.reqctr
                        ^
/home/ubuntu/px-fuse/pxd.c:223:18: note: in expansion of macro 'REQCTR'
  trace_pxd_reply(REQCTR(fc), req->in.h.unique, REQ_WRITE);
                  ^
/home/ubuntu/px-fuse/pxd.c: In function 'pxd_fuse_req':
/home/ubuntu/px-fuse/pxd.c:201:24: error: 'struct fuse_conn' has no member named 'iq'
 #define REQCTR(fc) (fc)->iq.reqctr
                        ^
/home/ubuntu/px-fuse/pxd.c:249:26: note: in expansion of macro 'REQCTR'
   trace_pxd_get_fuse_req(REQCTR(fc), nr_pages);
                          ^
/home/ubuntu/px-fuse/pxd.c:201:24: error: 'struct fuse_conn' has no member named 'iq'
 #define REQCTR(fc) (fc)->iq.reqctr
                        ^
/home/ubuntu/px-fuse/pxd.c:265:32: note: in expansion of macro 'REQCTR'
  trace_pxd_get_fuse_req_result(REQCTR(fc), status, eintr);
                                ^
/home/ubuntu/px-fuse/pxd.c: In function 'pxd_make_request':
/home/ubuntu/px-fuse/pxd.c:201:24: error: 'struct fuse_conn' has no member named 'iq'
 #define REQCTR(fc) (fc)->iq.reqctr
                        ^
/home/ubuntu/px-fuse/pxd.c:380:38: note: in expansion of macro 'REQCTR'
   pxd_dev->minor, bio->bi_rw, false, REQCTR(&pxd_dev->ctx->fc));
                                      ^
/home/ubuntu/px-fuse/pxd.c: In function 'pxd_rq_fn':
/home/ubuntu/px-fuse/pxd.c:201:24: error: 'struct fuse_conn' has no member named 'iq'
 #define REQCTR(fc) (fc)->iq.reqctr
                        ^
/home/ubuntu/px-fuse/pxd.c:452:41: note: in expansion of macro 'REQCTR'
    pxd_dev->minor, rq->cmd_flags, true, REQCTR(&pxd_dev->ctx->fc));
                                         ^
scripts/Makefile.build:258: recipe for target '/home/ubuntu/px-fuse/pxd.o' failed
make[2]: *** [/home/ubuntu/px-fuse/pxd.o] Error 1
Makefile:1384: recipe for target '_module_/home/ubuntu/px-fuse' failed
make[1]: *** [_module_/home/ubuntu/px-fuse] Error 2
make[1]: Leaving directory '/usr/src/linux-headers-4.4.0-040400-generic'
Makefile:34: recipe for target 'all' failed
make: *** [all] Error 2
--- FAIL: linux-headers-4.4.0-040400-generic (3.89s)
make -C /usr/src/linux-headers-4.4.0-040400-generic M=/home/ubuntu/px-fuse clean
make[1]: Entering directory '/usr/src/linux-headers-4.4.0-040400-generic'
  CLEAN   /home/ubuntu/px-fuse/.tmp_versions
make[1]: Leaving directory '/usr/src/linux-headers-4.4.0-040400-generic'
(Reading database ... 116922 files and directories currently installed.)
Removing linux-headers-4.4.0-040400-generic (4.4.0-040400.201601101930) ...
Removing linux-headers-4.4.0-040400 (4.4.0-040400.201601101930) ...
```

The script has built-in help:

**usage: kernel-tests.sh -huvn [regex-string]**

where
```
  -h  print this message
  -l  output to log file
  -n  perform scan for directories but DO NO actually test
  -s  specify regex to use to search for linux header directories [default: 4.[4567]]
  -u  use alternative URL to scan for linux headers [default: http://kernel.ubuntu.com/~kernel-ppa/mainline/]
  -v  verbose error messages
  [regex-string] is the optional string used to search the linux header 
  directory names to select them for testing [default: 4.[4567]]
```
