#!/bin/sh -xve
export NPROC=2

#export LD_RUN_PATH=/usr/local/lib:/opt/local/lib
#export LDFLAGS="-m64  -L/usr/local/lib -L/opt/local/lib -L/usr/lib/mps/64"
#export CPPFLAGS="-m64 -I/usr/local/include -I/opt/local/include"
#export CXXFLAGS="-m64"
#export CFLAGS="-m64"
#export LDFLAGS="-L/usr/local/lib -L/opt/local/lib -L/usr/lib/mps/64"
#export CPPFLAGS="-I/opt/local/include -I/opt/local/include"

if [ x"$1"x = x"--deps"x ]; then
    sudo ./install-deps.sh
fi

CEPH_DEV=true
if [ x"$CEPH_DEV"x != xx ]; then
    BUILDOPTS="$BUILDOPTS V=1 VERBOSE=1"
    CXX_FLAGS_DEBUG="-DCEPH_DEV"
    C_FLAGS_DEBUG="-DCEPH_DEV"
fi

#   To test with a new release Clang, use with cmake:
#	-D CMAKE_CXX_COMPILER="/usr/local/bin/clang++-devel" \
#	-D CMAKE_C_COMPILER="/usr/local/bin/clang-devel" \
COMPILE_FLAGS="-O0 -g"
#if [ `sysctl -n kern.osreldate` -le 1102000 ]; then
#    # We need to use the llvm linker for linking ceph-dencoder
#    COMPILE_FLAGS="$COMPILE_FLAGS -fuse-ld=/usr/bin/ld.lld"
#fi
CMAKE_CXX_FLAGS_DEBUG="$CXX_FLAGS_DEBUG $COMPILE_FLAGS"
CMAKE_C_FLAGS_DEBUG="$C_FLAGS_DEBUG $COMPILE_FLAGS"

CMAKE_CXX_FLAGS="-pthreads"
CMAKE_C_FLAGS=
CMAKE_EXE_LINKER_FLAGS=

for dir in /usr/local /opt/local; do
	CMAKE_CXX_FLAGS+=" -I$dir/include"
	CMAKE_EXE_LINKER_FLAGS+=" -L$dir/lib -Wl,-rpath $dir/lib"
done
CMAKE_EXE_LINKER_FLAGS+=" -L/usr/lib/mps -Wl,-rpath /usr/lib/mps"
CMAKE_SHARED_LINKER_FLAGS="$CMAKE_EXE_LINKER_FLAGS"

CMAKE_CXX_FLAGS_DEBUG="$CMAKE_CXX_FLAGS $CXX_FLAGS_DEBUG"
CMAKE_C_FLAGS_DEBUG="$CMAKE_C_FLAGS $C_FLAGS_DEBUG"

#
#   On FreeBSD we need to preinstall all the tools that are required for building
#   dashboard, because versions fetched are not working on FreeBSD.

rm -rf build

./do_cmake.sh "$*" \
	-D WITH_CCACHE=ON \
	-D CMAKE_BUILD_TYPE=Debug \
	\
	-D CMAKE_CXX_FLAGS_DEBUG="$CMAKE_CXX_FLAGS_DEBUG" \
	-D CMAKE_C_FLAGS_DEBUG="$CMAKE_C_FLAGS_DEBUG" \
	-D CMAKE_EXE_LINKER_FLAGS_DEBUG="$CMAKE_EXE_LINKER_FLAGS" \
	-D CMAKE_SHARED_LINKER_FLAGS_DEBUG="$CMAKE_SHARED_LINKER_FLAGS" \
	\
	-D CMAKE_CXX_FLAGS="$CMAKE_CXX_FLAGS" \
	-D CMAKE_C_FLAGS="$CMAKE_C_FLAGS" \
	-D CMAKE_EXE_LINKER_FLAGS="$CMAKE_EXE_LINKER_FLAGS" \
	-D CMAKE_SHARED_LINKER_FLAGS="$CMAKE_SHARED_LINKER_FLAGS" \
	\
	-D ENABLE_GIT_VERSION=OFF \
	-D WITH_SYSTEM_BOOST=ON \
	-D WITH_SYSTEM_NPM=ON \
	-D WITH_LTTNG=OFF \
	-D WITH_BABELTRACE=OFF \
	-D WITH_SEASTAR=OFF \
	-D WITH_BLKID=OFF \
	-D WITH_FUSE=OFF \
	-D WITH_KRBD=OFF \
	-D WITH_XFS=OFF \
	-D WITH_KVS=ON \
	-D CEPH_MAN_DIR=man \
	-D WITH_LIBCEPHFS=OFF \
	-D WITH_CEPHFS=OFF \
	-D WITH_MGR=YES \
	-D WITH_RDMA=OFF \
	-D WITH_SPDK=OFF \
	-D WITH_BLUESTORE=OFF \
	-D EXE_LINKER_USE_PIE=OFF \
	2>&1 | tee cmake.log
	# --debug-output --trace-expand \

echo start building 
date
(cd build; gmake -j$NPROC $BUILDOPTS )

#echo start testing 
#date
# And remove cores leftover from previous runs
#sudo rm -rf /tmp/cores.*
#(cd build; ctest -j$NPROC || ctest --rerun-failed --output-on-failure)
