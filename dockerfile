FROM ubuntu:14.04

RUN groupadd --gid 1000 skymizer \
    && useradd --uid 1000 --gid skymizer --shell /bin/bash --create-home skymizer \
    && echo 'skymizer ALL=(ALL:ALL) NOPASSWD:ALL' >> /etc/sudoers

RUN sed -i 's/archive.ubuntu.com/tw.archive.ubuntu.com/' /etc/apt/sources.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        curl \
        sudo \
        build-essential \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        libboost-all-dev \
        pkg-config \
        python3 \
        python3-pip \ 
        python3-setuptools \
        libgoogle-glog-dev \
        libgtest-dev \
        libiomp-dev \
        libleveldb-dev \ 
        liblmdb-dev \
        libopencv-dev \ 
        libopenmpi-dev \
        libsnappy-dev \
        libprotobuf-dev \ 
        openmpi-bin \
        openmpi-doc \
        libgflags-dev \ 
        automake \
        libtool \
        xz-utils \
        make \
        g++ \
        python \ 
        git \
        protobuf-compiler \
        libprotoc-dev \
        python-pip \
        python-dev \
        python-setuptools \
        bison \
        flex \
        ninja-build \
        gcovr \
        xz-utils \
        cmake-curses-gui \
    && pip install future numpy protobuf lit \
    && pip3 install jinja2 \
    && rm -rf /var/lib/apt/lists/*

RUN  curl https://cmake.org/files/v3.12/cmake-3.12.0.tar.gz --output cmake-3.12.0.tar.gz tar xvf cmake-3.12.0.tar.gz \
     &&  cd cmake-3.12.0 \
     && ./bootstrap \
     && make -j8 \
     && make install 
     
# fake logname output (needed by skymizer installer)
RUN echo 'id -nu' > $(which logname)

USER skymizer