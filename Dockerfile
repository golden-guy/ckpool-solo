# syntax=docker/dockerfile:1
FROM debian:11-slim

ENV BUILD_DIR=/var/build
ENV BIN_DIR=/srv/dockpool

ENV PUID=1000
ENV PGID=1000

# install ckpool-solo dependencies
RUN apt-get update && apt-get install -y autoconf automake libtool build-essential yasm libzmq3-dev libcap2-bin gosu

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
CMD /usr/sbin/gosu ${PUID}:${PGID} ./bin/ckpool -B -c ./conf/ckpool.conf
