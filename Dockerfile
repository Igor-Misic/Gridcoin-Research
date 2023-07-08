FROM ubuntu:20.04

LABEL maintainer="Igor Misic <igy1000mb@gmail.com>"

ENV TZ=Etc/UTC
RUN apt-get update && \
DEBIAN_FRONTEND=noninteractive apt-get -y --quiet --no-install-recommends install \
	ca-certificates \
	wget \
	git \
	make \
	python3 \
	automake \
	autoconf \
	libtool \
	pkg-config \
	libssl-dev \
	libboost-all-dev \
	libevent-dev \
	miniupnpc \
	libqrencode-dev \
	g++ \
	qt5-default \
	qtbase5-dev \
	qttools5-dev-tools \
	libqt5svg5 \
	bsdmainutils \
	libzip-dev \
	libcurl4-openssl-dev && \
	apt-get -y autoremove && \
	apt-get clean autoclean && \
	rm -rf /var/lib/apt/lists/{apt,dpkg,cache,log} /tmp/* /var/tmp/*
	
RUN wget -O /opt/linuxdeployqt https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage && \
	cd /opt/ && \
	chmod +x linuxdeployqt && \
	./linuxdeployqt --appimage-extract && \
	mv squashfs-root /opt/linuxdeployqtApp/ && \
	ln -s /opt/linuxdeployqtApp/AppRun /usr/local/bin/linuxdeployqt && \
	rm -fr squashfs-root && \
	rm linuxdeployqt

