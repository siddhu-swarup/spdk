# Storage Performance Development Kit

[![Go Doc](https://img.shields.io/badge/godoc-reference-blue.svg)](http://godoc.org/github.com/spdk/spdk/go/rpc)
[![Go Report Card](https://goreportcard.com/badge/github.com/spdk/spdk/go/rpc)](https://goreportcard.com/report/github.com/spdk/spdk/go/rpc)
[![PyPI Latest Release](https://img.shields.io/pypi/v/spdk.svg)](https://pypi.org/project/spdk/)
[![PyPI Downloads](https://img.shields.io/pypi/dm/spdk.svg?label=PyPI%20downloads)](https://pypi.org/project/spdk/)

NOTE: The SPDK mailing list has moved to a new location. Please visit
[this URL](https://lists.linuxfoundation.org/mailman/listinfo/spdk) to subscribe
at the new location. Subscribers from the old location will not be automatically
migrated to the new location.

The Storage Performance Development Kit ([SPDK](http://www.spdk.io)) provides a set of tools
and libraries for writing high performance, scalable, user-mode storage
applications. It achieves high performance by moving all of the necessary
drivers into userspace and operating in a polled mode instead of relying on
interrupts, which avoids kernel context switches and eliminates interrupt
handling overhead.

The development kit currently includes:

* [NVMe driver](http://www.spdk.io/doc/nvme.html)
* [I/OAT (DMA engine) driver](http://www.spdk.io/doc/ioat.html)
* [NVMe over Fabrics target](http://www.spdk.io/doc/nvmf.html)
* [iSCSI target](http://www.spdk.io/doc/iscsi.html)
* [vhost target](http://www.spdk.io/doc/vhost.html)
* [Virtio-SCSI driver](http://www.spdk.io/doc/virtio.html)

## In this readme

* [Documentation](#documentation)
* [Prerequisites](#prerequisites)
* [Source Code](#source)
* [Build](#libraries)
* [Unit Tests](#tests)
* [Vagrant](#vagrant)
* [AWS](#aws)
* [Advanced Build Options](#advanced)
* [Shared libraries](#shared)
* [Hugepages and Device Binding](#huge)
* [Example Code](#examples)
* [Contributing](#contributing)

<a id="documentation"></a>
## Documentation

[Doxygen API documentation](http://www.spdk.io/doc/) is available, as
well as a [Porting Guide](http://www.spdk.io/doc/porting.html) for porting SPDK to different frameworks
and operating systems.

<a id="source"></a>
## Source Code

~~~{.sh}
git clone https://github.com/spdk/spdk
cd spdk
git submodule update --init
~~~

<a id="prerequisites"></a>
## Prerequisites

The dependencies can be installed automatically by `scripts/pkgdep.sh`.
The `scripts/pkgdep.sh` script will automatically install the bare minimum
dependencies required to build SPDK.
Use `--help` to see information on installing dependencies for optional components

~~~{.sh}
./scripts/pkgdep.sh
~~~

<a id="libraries"></a>
## Build

Linux:

~~~{.sh}
./configure
make
~~~

FreeBSD:
Note: Make sure you have the matching kernel source in /usr/src/ and
also note that CONFIG_COVERAGE option is not available right now
for FreeBSD builds.

~~~{.sh}
./configure
gmake
~~~

<a id="tests"></a>
## Unit Tests

~~~{.sh}
./test/unit/unittest.sh
~~~

You will see several error messages when running the unit tests, but they are
part of the test suite. The final message at the end of the script indicates
success or failure.

<a id="vagrant"></a>
## Vagrant

A [Vagrant](https://www.vagrantup.com/downloads.html) setup is also provided
to create a Linux VM with a virtual NVMe controller to get up and running
quickly.  Currently this has been tested on MacOS, Ubuntu 16.04.2 LTS and
Ubuntu 18.04.3 LTS with the VirtualBox and Libvirt provider.
The [VirtualBox Extension Pack](https://www.virtualbox.org/wiki/Downloads)
or [Vagrant Libvirt] (https://github.com/vagrant-libvirt/vagrant-libvirt) must
also be installed in order to get the required NVMe support.

Details on the Vagrant setup can be found in the
[SPDK Vagrant documentation](http://spdk.io/doc/vagrant.html).

<a id="aws"></a>
## AWS

The following setup is known to work on AWS:
Image: Ubuntu 18.04
Before running  `setup.sh`, run `modprobe vfio-pci`
then: `DRIVER_OVERRIDE=vfio-pci ./setup.sh`

<a id="advanced"></a>
## Advanced Build Options

Optional components and other build-time configuration are controlled by
settings in the Makefile configuration file in the root of the repository. `CONFIG`
contains the base settings for the `configure` script. This script generates a new
file, `mk/config.mk`, that contains final build settings. For advanced configuration,
there are a number of additional options to `configure` that may be used, or
`mk/config.mk` can simply be created and edited by hand. A description of all
possible options is located in `CONFIG`.

Boolean (on/off) options are configured with a 'y' (yes) or 'n' (no). For
example, this line of `CONFIG` controls whether the optional RDMA (libibverbs)
support is enabled:

~~~{.sh}
CONFIG_RDMA?=n
~~~

To enable RDMA, this line may be added to `mk/config.mk` with a 'y' instead of
'n'. For the majority of options this can be done using the `configure` script.
For example:

~~~{.sh}
./configure --with-rdma
~~~

Additionally, `CONFIG` options may also be overridden on the `make` command
line:

~~~{.sh}
make CONFIG_RDMA=y
~~~

Users may wish to use a version of DPDK different from the submodule included
in the SPDK repository.  Note, this includes the ability to build not only
from DPDK sources, but also just with the includes and libraries
installed via the dpdk and dpdk-devel packages.  To specify an alternate DPDK
installation, run configure with the --with-dpdk option.  For example:

Linux:

~~~{.sh}
./configure --with-dpdk=/path/to/dpdk/x86_64-native-linuxapp-gcc
make
~~~

FreeBSD:

~~~{.sh}
./configure --with-dpdk=/path/to/dpdk/x86_64-native-bsdapp-clang
gmake
~~~

The options specified on the `make` command line take precedence over the
values in `mk/config.mk`. This can be useful if you, for example, generate
a `mk/config.mk` using the `configure` script and then have one or two
options (i.e. debug builds) that you wish to turn on and off frequently.

<a id="shared"></a>
## Shared libraries

By default, the build of the SPDK yields static libraries against which
the SPDK applications and examples are linked.
Configure option `--with-shared` provides the ability to produce SPDK shared
libraries, in addition to the default static ones.  Use of this flag also
results in the SPDK executables linked to the shared versions of libraries.
SPDK shared libraries by default, are located in `./build/lib`.  This includes
the single SPDK shared lib encompassing all of the SPDK static libs
(`libspdk.so`) as well as individual SPDK shared libs corresponding to each
of the SPDK static ones.

In order to start a SPDK app linked with SPDK shared libraries, make sure
to do the following steps:

- run ldconfig specifying the directory containing SPDK shared libraries
- provide proper `LD_LIBRARY_PATH`

If DPDK shared libraries are used, you may also need to add DPDK shared
libraries to `LD_LIBRARY_PATH`

Linux:

~~~{.sh}
./configure --with-shared
make
ldconfig -v -n ./build/lib
LD_LIBRARY_PATH=./build/lib/:./dpdk/build/lib/ ./build/bin/spdk_tgt
~~~

<a id="huge"></a>
## Hugepages and Device Binding

Before running an SPDK application, some hugepages must be allocated and
any NVMe and I/OAT devices must be unbound from the native kernel drivers.
SPDK includes a script to automate this process on both Linux and FreeBSD.
This script should be run as root.

~~~{.sh}
sudo scripts/setup.sh
~~~

Users may wish to configure a specific memory size. Below is an example of
configuring 8192MB memory.

~~~{.sh}
sudo HUGEMEM=8192 scripts/setup.sh
~~~

There are a lot of other environment variables that can be set to configure
setup.sh for advanced users. To see the full list, run:

~~~{.sh}
scripts/setup.sh --help
~~~

<a id="targets"></a>
## Target applications

After completing the build process, SPDK target applications can be found in
`spdk/build/bin` directory:

* [nvmf_tgt](https://spdk.io/doc/nvmf.html) - SPDK NVMe over Fabrics target
  presents block devices over a fabrics,
* [iscsi_tgt](https://spdk.io/doc/iscsi.html) - SPDK iSCSI target runs I/O
  operations remotely with TCP/IP protocol,
* [vhost](https://spdk.io/doc/vhost.html) - A vhost target provides a local
  storage service as a process running on a local machine,
* spdk_tgt - combines capabilities of all three applications.

SPDK runs in a polled mode, which means it continuously checks for operation completions.
This approach assures faster response than interrupt mode, but also lessens usefulness
of tools like `top`, which only shows 100% CPU usage for SPDK assigned cores.
[spdk_top](https://spdk.io/doc/spdk_top.html) is a program which simulates `top` application
and uses SPDK's [JSON RPC](https://spdk.io/doc/jsonrpc.html) interface to present statistics
about SPDK threads, pollers and CPU cores as an interactive list.

<a id="examples"></a>
## Example Code

Example code is located in the examples directory. The examples are compiled
automatically as part of the build process. Simply call any of the examples
with no arguments to see the help output. You'll likely need to run the examples
as a privileged user (root) unless you've done additional configuration
to grant your user permission to allocate huge pages and map devices through
vfio.

<a id="python"></a>
## Python bindings

SPDK python bindings and scripts are located in [python](./python) folder. Python code is
automatically built as part of the build process. Python package is also published
to <https://pypi.org/project/spdk/> every release for ease of consumption. For more
details, check the [README](./python/README.md).

<a id="contributing"></a>
## Contributing

For additional details on how to get more involved in the community, including
[contributing code](http://www.spdk.io/development) and participating in discussions and other activities, please
refer to [spdk.io](http://www.spdk.io/community)
