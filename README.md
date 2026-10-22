# DeskOrganization

Windows 11 桌面整理工具。桌面上开一块块盒子，文件拖进去就归档到对应文件夹，桌面立刻干净；
每一步都能撤销。

## 构建

需要 Visual Studio 2022 的 C++ 工作负载（含 Windows SDK）与自带的 CMake、Ninja。

    scripts\build.cmd

产物在 out\build\DeskOrganization.exe。清理后重新配置用 scripts\build.cmd clean。

发布用的产物用 scripts\package.cmd：先按 Release 配置编译（开优化，C 运行库静态链接进 exe，
所以免安装版在没有装 VC++ 运行库的机器上也能直接双击），再压出免安装 zip、编出安装包，
两者都放在 out\dist\。安装包需要另外安装 Inno Setup 6，脚本按 %ProgramFiles(x86)%\Inno Setup 6 找它，装在别处就用
DESKORG_ISCC 指过去。编译器和 CMake、Ninja 的位置由脚本自己找（先问 vswhere，再用 PATH），
所以装 Build Tools 或者完整版 Visual Studio 都能直接编。
发布版本号写在 CMakeLists.txt 的 DESKORG_VERSION 里，格式 a.b.c，只此一处：exe 内的版本
资源、免安装包的压缩包名、安装包里显示的版本都由它生成，配置时抄到构建目录的 version.txt
供打包脚本读，所以不会出现包名和程序属性页版本不一致。

正式发布的包由 GitHub Actions 在 windows-latest 上构建：推一个 vX.Y.Z 标签，工作流跑
scripts\package.cmd 出两个包，再用 Release 版跑一遍自检，核对标签和 DESKORG_VERSION 对得上，
然后把安装包和 zip 挂到该标签的 Release 上。标签和版本号不一致会直接失败，不会发出
名字和内容对不上的包。发布说明取仓库根目录的 RELEASE-NOTES.md。

程序自己的图标在 resources\DeskOrganization.ico，画法和托盘图标共用 src\iconres.cpp 里那套
像素函数（16/20/24/32/40/48/64/128 八档，.ico 里用位图条目，rc.exe 不收 PNG 压缩的图标）。
要换配色或改尺寸就跑 scripts\make_icon.cmd，它把两张 .ico 和各档 PNG 导到 out\ 下，
看过满意再覆盖 resources 里那一份。

## 使用

直接运行 DeskOrganization.exe 即可，程序常驻托盘，只有一个实例。

- 新建盒子：托盘菜单。拖动盒子标题栏移动位置，双击标题栏或右键菜单收起、展开。
- 归档文件：从资源管理器或桌面把文件拖到盒子上，文件会被移动到该盒子对应的目录。
- 撤销：托盘菜单或盒子右键菜单，也可以从桌面右键菜单撤销（默认不开启，在托盘菜单里勾选）。
  撤销历史会保存，重启程序后仍能撤销上一次的整理。
- 重命名：盒子右键菜单里选“重命名”，直接在标题栏输入，回车确认，Esc 或点到别处放弃。
- 合并成标签盒：盒子右键菜单里选“与其他盒子合并成标签盒”，弹出的菜单上每个盒子一项，勾选即合并、再点一次移出，可以连着勾好几个。
  已经是标签盒的不能当成员，所以不会出现套两层的标签卡；被合并的盒子暂时隐藏，全部移出后各自回来。
- 盒子里看子目录：盒子对应的目录里有子文件夹时，标签卡下面会多出第二排——「本层」加各个子文件夹。点其中一个就在盒子内看那一层，
  拖进来的文件也落在这一层；再深的目录只作为条目显示，双击交给资源管理器。第二排不影响自动整理规则，规则永远归到盒子的根目录。
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
    sub =
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

sub 是盒子里当前显示的第二层，也就是 folder 下面那个子文件夹的名字，留空表示盒子自己那一层。
这一排由程序按实际的子文件夹列出来，手写的值必须真有这么个目录，否则按留空处理。点第二排的
标签卡会改写这一行；它只决定显示哪一层、以及新拖进来的文件落在哪，不会搬动已经在盒子里的文件。

规则按后缀、文件名前缀、文件名包含三种条件匹配，大小写不敏感，第一条命中的规则生效；
ext、prefix、contains 至少要写一个，全空的规则不匹配任何东西。folders = 1 才把文件夹一起归。
sweep = 1 表示连桌面上已有的文件一起整理，sweep = 0 只处理这条规则建立之后新出现的文件。

theme 取 auto、light 或 dark，auto 跟随系统深浅色，切换后不用重启程序。

改分辨率或者拔掉显示器之后，落在屏幕外的盒子会在启动时或者显示设置变化时被拉回最近的显示器，
至少留一小块标题栏可以点住，盒体允许超出屏幕边缘。

## 已知限制

- 只支持 Windows。桌面层级、图标位置这些都依赖 Win32 与 Shell 的具体行为，在其他系统上没有对应物。
- 盒子只显示两层：盒子目录本身，和它的子文件夹（第二排标签卡）。再深一层只作为条目显示、双击在
  资源管理器里打开，不会在盒子内继续展开；条目多了靠滚轮按行翻，没有缩略图预览。
- 只备份桌面图标的坐标。视图方式、图标大小、排序这些桌面设置不在备份范围内。
- 自动整理规则会把桌面上已经存在的文件也归类过去，包括你自己刚放下的；撤销能退回去，
  且刚退回的文件在 60 秒内不会被规则再次收走。

## 授权

MIT，见 LICENSE。
