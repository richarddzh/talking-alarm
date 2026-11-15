# 小鱼缸／番茄时钟原图处理

依赖：Python 3.10+、Pillow。共享环境位于 `python_scripts\.venv`；
不要为本任务单独创建环境。

```powershell
python_scripts\.venv\Scripts\python.exe -m pip install -r python_scripts\requirements.txt
python_scripts\.venv\Scripts\python.exe python_scripts\prepare_app_art\prepare_app_art.py
python_scripts\.venv\Scripts\python.exe tools\generate_launcher_icons.py
```

输入为 `esp32_idf_s3n16r8\main\assets\small_apps\references` 内用户提供的
原始图片，完整保留，不覆盖源文件。先按已知背景色连通抠图并选中主体，
再做 3×3 中值滤波、96 色轻度归并及弱反锐化；最后等比例缩放、透明留边。
不重绘鱼身、花纹、鳍、眼睛或番茄表情。番茄底部灰白页面条从 alpha
排除，不用生成式填补；完整原图仍保留。番茄时钟参考图直接缩放，
按用户要求，表盘内部作为透明功能层，用 1–12 数字和实际时间指针替换
原短线刻度与固定指针；番茄外壳、轮廓和配色不重画。

输出包含可检查的透明 PNG、启动器 48×48 图标输入及 RGB565/alpha 的
`small_apps_art.h`，运行时不需要解码 PNG。鱼的水平翻转只用于朝向。
图标生成器仍保留原有五个图标的处理方式。
鱼虫参考图保存在 `references\food_worm.png`，只去除外部连通的灰白棋盘背景，
保留黑色轮廓内的白眼睛和绿色身体。鱼为 40×30，鱼虫为 14×14 透明留边图，
按原比例缩放，不单独夸大眼睛或修改身体。鱼虫 RGB565/alpha 另占 588 B Flash。

参考图中外围装饰和页面标记保留在完整原件；主体 cutout 用于精灵。
源图来自用户附件，不宣称其为原创或无版权；分发者须确认有权使用。
