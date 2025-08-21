IMAGE=flashtool_builder
DOCKER=docker run -t --rm \
	   -v `pwd`:/home/ubuntu/mtkft \
	   -w /home/ubuntu/mtkft \
	   -u $$(id -u):$$(id -g) \
	   $(IMAGE)

prepare:
	cat Dockerfile | docker build -t $(IMAGE) -

build: windows

windows:
	$(DOCKER) bash -c "\
			  	   		rm -rf ./cmake-build-release-docker-mingw15/ CMakeCache.txt ; \
			  			mkdir -p ./cmake-build-release-docker-mingw15 && \
			  			cd cmake-build-release-docker-mingw15 && \
			  			cmake -DCMAKE_TOOLCHAIN_FILE=../TC-mingw.cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. && \
			  			ninja \
			  		   "

linux_docker:
	$(DOCKER) bash -c "\
			  	   		rm -rf ./cmake-build-release-linux/ CMakeCache.txt ; \
			  			mkdir ./cmake-build-release-linux && \
			  			cd cmake-build-release-linux && \
			  			CC=gcc-13 cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. && \
			  			ninja \
			  		   "

linux:
	-rm -rf build
	mkdir build
	cd build && cmake .. && make

.DEFAULT_GOAL := windows
.PHONY: build linux_docker windows prepare linux
