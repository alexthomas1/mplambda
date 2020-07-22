#  Copyright 2019 U.C. Berkeley RISE Lab
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.

FROM ubuntu:18.04


# Hydrobase
MAINTAINER Vikram Sreekanti <vsreekanti@gmail.com> version: 0.1
USER root

ARG repo_org=hydro-project
ARG source_branch=master
ARG build_branch=docker-build

RUN apt-get update
RUN apt-get install -y build-essential autoconf automake libtool curl make \
      unzip pkg-config wget curl git vim jq software-properties-common \
      libzmq3-dev git gcc libpq-dev libssl-dev \
      openssl libffi-dev zlib1g-dev net-tools

# Install clang-5 and other related C++ things; this uses --force-yes because 
# of some odd disk space warning that I can't get rid of.
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key| apt-key add - \
  && apt-add-repository "deb http://apt.llvm.org/buster/ llvm-toolchain-buster main" \
  && apt-get update \
  && apt-get install -y clang-9 lldb-9

# Update the default choice for clant to be clang-5.0.
RUN update-alternatives --install /usr/bin/clang clang /usr/bin/clang-9 1
RUN update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-9 1
RUN apt-get install -y libc++-dev libc++abi-dev 


# Install Cmake
RUN wget "https://cmake.org/files/v3.15/cmake-3.15.4-Linux-x86_64.tar.gz" && \
    tar xvzf cmake-3.15.4-Linux-x86_64.tar.gz && \
    mv cmake-3.15.4-Linux-x86_64 /usr/bin/cmake && \
    rm /cmake-3.15.4-Linux-x86_64.tar.gz 
ENV PATH $PATH:/usr/bin/cmake/bin
RUN cmake --version 


# Install python3.6. We use a separate apt repository because Ubuntu 14.04 does
# not come with this version of Python3 enabled.
RUN apt-get update
RUN apt-get install -y python3-distutils
RUN apt-get install -y python3-pip
RUN pip3 install awscli cloudpickle zmq protobuf boto3 kubernetes six


# Download protobuf 3.5.1.
RUN wget https://github.com/google/protobuf/releases/download/v3.5.1/protobuf-all-3.5.1.zip
RUN unzip protobuf-all-3.5.1.zip 

# Build and install protobuf. NOTE: this step takes a really long time!
WORKDIR /protobuf-3.5.1/
RUN ./autogen.sh
# RUN ./configure CXX=clang++ CXXFLAGS='-std=c++11 -stdlib=libc++ -O3 -g'
RUN ./configure
RUN make -j4
RUN make check -j4
RUN make install
RUN ldconfig

# Clean up protobuf install files.
WORKDIR /
RUN rm -rf protobuf-3.5.1 protobuf-all-3.5.1.zip

# Create and populate Hydro project context.
RUN mkdir /hydro
ENV HYDRO_HOME /hydro
WORKDIR /hydro

# Clone all the Hydro project repos here.
RUN git clone --recurse-submodules https://github.com/keplerc/anna -b TopK
RUN git clone --recurse-submodules https://github.com/cw75/anna-cache
RUN git clone --recurse-submodules https://github.com/cw75/cloudburst -b TopK
RUN git clone --recurse-submodules https://github.com/cw75/cluster -b TopK

WORKDIR /



# True Docker File 

# Download latest version of the code from relevant repository & branch -- if
# none are specified, we use hydro-project/cloudburst by default. Install the KVS
# client from the Anna project.
WORKDIR $HYDRO_HOME/cloudburst
#RUN git remote remove origin && git remote add origin https://github.com/$repo_org/cloudburst
#RUN git fetch -p origin && git checkout -b $build_branch origin/$source_branch
RUN rm -rf /usr/lib/python3/dist-packages/yaml
RUN rm -rf /usr/lib/python3/dist-packages/PyYAML-*
RUN pip3 install -r requirements.txt
WORKDIR $HYDRO_HOME
RUN rm -rf anna
RUN git clone --recurse-submodules https://github.com/keplerc/anna -b TopK
WORKDIR anna
RUN cd client/python && python3.6 setup.py install
WORKDIR /

# These installations are currently pipeline specific until we figure out a
# better way to do package management for Python.
RUN pip3 install tensorflow==1.12.0 tensorboard==1.12.2 scikit-image sortedcontainers

RUN pip3 install pandas s3fs 

RUN touch a
RUN pip3 install --upgrade git+https://github.com/devin-petersohn/modin@engines/cloudburst_init

WORKDIR /
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
    && apt-get install -y software-properties-common \
    && add-apt-repository -y ppa:deadsnakes/ppa \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update
# Install gcc-9, zeroMQ (libzmq3-dev), python3, curl
RUN apt install -y sudo libassimp-dev libcurl4-openssl-dev libspdlog-dev libcurl4-openssl-dev libeigen3-dev ninja-build
        
#RUN apt install -y sudo build-essential subversion python3-dev \
#        libncurses5-dev libxml2-dev libedit-dev swig doxygen graphviz xz-utils \
#        gcc-9 g++ python3-pip curl libzmq3-dev git libc++-dev libc++abi-dev libassimp-dev \
#        autoconf automake libtool make unzip pkg-config wget libpq-dev openssl libffi-dev zlib1g-dev \
#        clang-5.0 lldb-5.0 libcurl4-openssl-dev libssl-dev clang++ libcurl4-openssl-dev libeigen3-dev ninja-build


#Libccd 
WORKDIR /
RUN git clone https://github.com/UNC-Robotics/nigh.git && \
    git clone https://github.com/keplerc/anna.git -b TopK && \
    git clone https://github.com/danfis/libccd.git 
WORKDIR /libccd
RUN mkdir build && cd build && \
    cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release .. && \
    make -j8 && make install


WORKDIR /
RUN git clone https://github.com/flexible-collision-library/fcl.git && \
    cd fcl && mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && make -j8 && make install  
    
RUN apt install -y emacs-nox

RUN git clone https://github.com/awslabs/aws-lambda-cpp.git && \
    cd aws-lambda-cpp && \
    mkdir build && \
    cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=~/lambda-install && \
    make -j8 && make install

WORKDIR /
RUN git clone https://github.com/aws/aws-sdk-cpp.git && \
    cd aws-sdk-cpp && \
    mkdir build && cd build && \
    cmake .. -DBUILD_ONLY="s3;lambda" \
    -DENABLE_UNITY_BUILD=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=~/aws && \
    make -j8 && make install

#Anna
WORKDIR /anna
RUN git submodule init && git submodule update
RUN bash scripts/build.sh -j4 -bRelease -g

RUN echo "rebuild4"
WORKDIR /
RUN git clone https://github.com/KeplerC/mplambda.git -b TopK 


WORKDIR /mplambda
RUN mkdir -p build/ && \
    cd build/ && \
    cmake -DCMAKE_BUILD_TYPE=Debug .. -DCMAKE_PREFIX_PATH="~/aws;~/lambda-install" -DBUILD_SHARED_LIBS=ON && \
    make -j8 
RUN mkdir -p build/

WORKDIR /
COPY start-cloudburst.sh /start-cloudburst.sh
CMD bash start-cloudburst.sh
