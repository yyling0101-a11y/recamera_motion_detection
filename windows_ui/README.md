# Windows PySide6 区域标注工具

## 安装

建议在 Anaconda/venv 环境中运行：

```powershell
cd windows_ui
python -m pip install -r requirements.txt
```

## 启动

```powershell
python app.py `
  --rtsp rtsp://192.168.42.1:8554/live0 `
  --api http://192.168.42.1:8080
```

也可双击 `run_windows.bat`，然后在界面中修改地址。

## 使用顺序

1. 输入 RTSP URL，点击“连接 RTSP”。
2. 选择“检测区域”，在画面上依次左键添加多边形顶点。
3. 点击“完成当前多边形”。
4. 对风扇、树叶、显示屏等长期运动位置，选择“白名单/忽略区域”再画多边形。
5. 点击“上传到设备”。
6. 等待背景预热结束，右下状态会显示是否检测到运动。

操作：

- 左键：增加顶点
- 右键、Backspace 或 Z：撤销最后一个顶点
- Esc：“取消当前绘制”
- 区域列表：选择后可删除

区域坐标以 0~1 保存，与窗口缩放和 RTSP 分辨率无关。
