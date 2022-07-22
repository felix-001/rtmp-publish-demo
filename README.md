# 目录结构介绍
- libs目录是编译好的推流sdk的库(.a文件)
- media目录里面包含用来模拟ipc的h264和aac文件
- src目录包含demo的代码，其中`ipc_simulator.c`不需要用户去关注，这个只是用文件来模拟ipc，与sdk的使用姿势无关
- includes目录是sdk的头文件的目录

# 编译
- 创建`build`目录
```
mkdir build
cd build
```

- 生成Makefile文件
```
cmake .. -DARCH=tda2 -DCROSS_COMPILE=xxx
```
`CROSS_COMPILE`这个参数为交叉编译工具链的前缀，比如工具链为`arm-linux-gnueabi-gcc`,则命令为
```
cmake .. -DARCH=tda2 -DCROSS_COMPILE=arm-linux-gnueabi-
```

如果是x86平台:
```
cmake .. -DARCH=x86
```


 - 编译
```
make
```

# 运行
1. 将media目录下的文件拷贝到和`rtmp-publish-demo`同级目录下

```
cp ../media/* .
```

2. 运行
```
./rtmp-publish-demo <rtmp推流地址>
```
