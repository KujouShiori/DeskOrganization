# DeskOrganization

Windows 11 桌面整理工具。桌面上开一块块盒子，文件拖进去就归档到对应文件夹，桌面立刻干净；
每一步都能撤销。

## 构建

需要 Visual Studio 2022 的 C++ 工作负载（含 Windows SDK）与自带的 CMake、Ninja。

    scripts\build.cmd

产物在 out\build\DeskOrganization.exe。清理后重新配置用 scripts\build.cmd clean。

## 使用

直接运行 DeskOrganization.exe 即可，程序常驻托盘，只有一个实例。

- 新建盒子：托盘菜单。拖动盒子标题栏移动位置，双击标题栏或右键菜单收起、展开。
- 归档文件：从资源管理器或桌面把文件拖到盒子上，文件会被移动到该盒子对应的目录。
- 撤销：托盘菜单或盒子右键菜单，也可以从桌面右键菜单撤销（默认不开启，在托盘菜单里勾选）。
  撤销历史会保存，重启程序后仍能撤销上一次的整理。
- 重命名：盒子右键菜单里选“重命名”，直接在标题栏输入，回车确认，Esc 或点到别处放弃。
- 隐藏桌面原生图标：托盘菜单。程序异常退出后下次启动会把图标恢复回来。
- 备份桌面图标布局：托盘菜单，还原时按图标名字对位。

## 自检

    out\build\DeskOrganization.exe --selftest

跑全部断言，不移动任何桌面图标，也不改你的文件；结果写在当前目录的 selftest.txt。

    out\build\DeskOrganization.exe --layouttest

会真的移动一个桌面图标再放回原位，用来验证坐标读写和还原这条链路。桌面是共享状态，
所以它不在默认自检里，只在你需要确认这部分时手动跑。

## 配置

配置在 %APPDATA%\DeskOrganization\layout.ini，UTF-8 编码，可以直接手改，注释会被保留。
程序另存两个自己维护的文件：undo-history.txt（撤销历史）和 organize-baseline.txt
（记录 sweep = 0 的规则建立时桌面上已有哪些文件），不需要手改。

    [general]
    hide_icons = 0
    box_root =
    theme = auto

    [box:图片]
    left = 40
    top = 40
    width = 340
    height = 260
    collapsed = 0
    folder =
    grouped = 0
    tab =
    members =

    [rule:按后缀归档]
    box = 图片
    ext = .png,.jpg
    prefix =
    contains =
    folders = 0
    sweep = 1

box_root 是盒子目录的根，留空就是桌面。盒子的 folder 写成绝对路径就原样使用，写成相对路径则
相对 box_root 解析，留空时用盒子的名字当目录名。改完保存后重启程序生效。

grouped = 1 让盒子按文件类型分标签卡显示，这只是分组显示，不移动任何文件。members 填逗号
分隔的盒子名，就把这个盒子变成多标签盒子：每个标签卡显示一个成员盒子的目录，成员盒子本身会被
隐藏，删掉 members 那一行它们各自回来，位置不变。

规则按后缀、文件名前缀、文件名包含三种条件匹配，大小写不敏感，第一条命中的规则生效；
ext、prefix、contains 至少要写一个，全空的规则不匹配任何东西。folders = 1 才把文件夹一起归。
sweep = 1 表示连桌面上已有的文件一起整理，sweep = 0 只处理这条规则建立之后新出现的文件。

theme 取 auto、light 或 dark，auto 跟随系统深浅色，切换后不用重启程序。

改分辨率或者拔掉显示器之后，落在屏幕外的盒子会在启动时或者显示设置变化时被拉回最近的显示器，
至少留一小块标题栏可以点住，盒体允许超出屏幕边缘。

## 已知限制

- 只支持 Windows。桌面层级、图标位置这些都依赖 Win32 与 Shell 的具体行为，在其他系统上没有对应物。
- 盒子只显示它那个目录里的一层内容，子目录会作为条目显示、双击在资源管理器里打开，但不会在盒子内展开；
  条目多了靠滚轮按行翻，没有缩略图预览。
- 只备份桌面图标的坐标。视图方式、图标大小、排序这些桌面设置不在备份范围内。
- 自动整理规则会把桌面上已经存在的文件也归类过去，包括你自己刚放下的；撤销能退回去，
  且刚退回的文件在 60 秒内不会被规则再次收走。

## 授权

MIT，见 LICENSE。
