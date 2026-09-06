# 模拟摇杆与二维焦点导航设计

## 硬件输入

摇杆提供三路信号：

- `B`：低电平有效的数字按键；
- `X`：ADC 模拟横轴；
- `Y`：ADC 模拟纵轴。

最终接线映射为 B/GPIO21、X/GPIO20、Y/GPIO19。X/Y 不能进入数字按键去抖
数组，也不能直接产生 `GUI_INPUT_ACTIVATE`。

`buttons.c` 启动时对每个轴采样 32 次建立中心值。方向判定使用两级阈值：

- 偏离中心 850 ADC counts：进入方向状态并发出一次事件；
- 回到中心 500 counts 内：释放方向状态；
- 保持拨动 350 ms 后开始重复，每 140 ms 重复一次。

两级阈值形成迟滞，避免中心噪声导致焦点抖动。安装方向相反时使用
`APP_JOYSTICK_X_INVERTED` / `APP_JOYSTICK_Y_INVERTED`，不在业务代码里
交换方向。

B 使用独立的 60 ms 去抖，并要求 X/Y 连续保持中立至少 100 ms。输入循环
总是先读取两个 ADC 轴，再判断 B；轴偏转或刚回中时出现的 B 低电平毛刺会
被记录并丢弃，不会进入 GUI 确认路径。

## 事件隔离

输入适配层只输出：

```text
MAIN_PRESSED / MAIN_RELEASED
JOYSTICK_PRESSED
LEFT / RIGHT / UP / DOWN
```

`JOYSTICK_PRESSED` 来自 B，方向事件只来自 ADC。独立主按钮转换为
`GUI_INPUT_ACTIVATE`，摇杆 B 转换为独立的 `GUI_INPUT_AUX_ACTIVATE`。
Shell 的默认行为把两者都当确认，但 App 可以优先接管辅助确认。X/Y 拨动
在类型层面无法触发任一种按钮动作。

## 通用焦点邻接表

每个 App 为所有可聚焦控件定义一个 `gui_focus_node_t[]`：

```c
static const gui_focus_node_t focus_grid[] = {
    {.left = GUI_FOCUS_HOME, .right = 1,
     .up = GUI_FOCUS_NONE, .down = 2},
};
```

每个节点显式声明四个方向的目标：

- 非负值：目标控件索引；
- `GUI_FOCUS_HOME`：底部首页按钮；
- `GUI_FOCUS_NONE`：该方向保持原焦点。

这比按数组顺序循环更适合不规则布局，也让新增按钮或调整排版时的导航
行为可审查、可预测。主界面的三列 App 网格由 Shell 使用相同二维语义计算。

## 事件循环

- `app_main` 读取硬件事件并交给 `gui_shell_handle_input()`；
- 主界面打开时，由 Shell 的主界面输入响应函数处理；
- App 打开时，先调用当前 App 的 `handle_input()`；
- 未被 App 消费的方向事件再进入通用焦点邻接表；
- `gui_shell_tick()` 只运行当前 App 的 `tick()`。

俄罗斯方块和贪吃蛇运行时接管方向事件；停止时方向事件恢复为开始按钮和
首页之间的焦点移动。所有逻辑运行在 Core 0，Core 1 继续专用于高优先级音频。
