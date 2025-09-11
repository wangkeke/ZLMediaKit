1. git clone --depth 1 https://github.com/wangkeke/ZLMediaKit.git
2. cd ZLMediaKit
3. git submodule update --init
4. docker build --build-arg MODEL=Release -t zlmediakit:opus .
