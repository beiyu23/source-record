# OBS Studio的Source Record filter

obs studio的插件source record

# 原作者下载地址

https://obsproject.com/forum/resources/source-record.1285/

# obs构建和插件开发

1. obs内构建（windows平台，有点麻烦）
   * 构建OBS Studio：[https://obsproject.com/wiki/Install-Instructions](https://obsproject.com/wiki/Install-Instructions)
   * 把插件改名为source-record,并放置到plugins目录下，目录构成应该是plugins/source-record
   * 在plugins/CMakeLists.txt中添加 `add_subdirectory(source-record)`
   * 重新构建 OBS Studio
2. 独立构建（仅限 Linux）//我没试过
   * 确认您具有包含 OBS 开发文件的包
   * 查看此存储库并运行 `cmake -S . -B build -DBUILD_OUT_OF_TREE=On && cmake --build build` `cmake -S . -B build -DBUILD_OUT_OF_TREE=On && cmake --buid build`
