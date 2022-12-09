# syntax=docker/dockerfile:1
FROM debian:11-slim

ENV BUILD_DIR=/var/build
ENV BIN_DIR=/srv/dockpool

ENV USER_UID=1000
ENV USER_GID=1000
ENV USER_NAME=dockpool

# create the dockpool group and user
RUN groupadd --gid ${USER_GID} ${USER_NAME} \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m ${USER_NAME}

# install ckpool-solo dependencies
RUN apt-get update && apt-get install -y autoconf automake libtool build-essential yasm libzmq3-dev libcap2-bin

# build ckpool-solo sources
COPY . ${BUILD_DIR}
WORKDIR ${BUILD_DIR}
RUN ./autogen.sh && ./configure --prefix=${BIN_DIR}
RUN make

# install binaries
RUN make install
RUN mkdir -p ${BIN_DIR}/conf
COPY ckpool.conf ${BIN_DIR}/conf

# final configuration
EXPOSE 3333
WORKDIR ${BIN_DIR}

# switch to dockpool user
USER ${USER_NAME}

# start ckpool
CMD ./bin/ckpool -B -c ./conf/ckpool.conf
