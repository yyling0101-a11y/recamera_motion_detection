(() => {
  const root = document.getElementById("app");
  const style = document.createElement("style");
  style.textContent = `body{margin:0;color:#263238;font:14px system-ui,sans-serif}section{padding:16px;border:1px solid #d5dee2;border-radius:10px;background:#fff}h2{margin:0 0 8px;font-size:19px}p{margin:8px 0}button{padding:9px 14px;border:0;border-radius:7px;background:#1976d2;color:#fff;cursor:pointer}.muted{color:#62747e}.status{display:inline-block;padding:3px 9px;border-radius:30px;background:#e8f5e9;color:#236335}`;
  document.head.appendChild(style);
  recamera.onState = (state) => {
    root.replaceChildren();
    const section = document.createElement("section");
    const title = document.createElement("h2");
    title.textContent = state.name;
    const description = document.createElement("p");
    description.textContent = state.description;
    const status = document.createElement("p");
    status.className = "status";
    status.textContent = state.running ? "● Running / 运行中" : "○ Waiting for app / 等待应用";
    const zones = document.createElement("p");
    zones.className = "muted";
    zones.textContent = `Zones / 区域: ${state.zones.length} · Motion / 运动: ${state.motion ? "Yes / 是" : "No / 否"}`;
    const button = document.createElement("button");
    button.textContent = "Edit zones and sensitivity / 编辑选区与灵敏度";
    button.onclick = () => recamera.openMotionEditor();
    section.append(title, description, status, zones, button);
    root.append(section);
  };
})();
