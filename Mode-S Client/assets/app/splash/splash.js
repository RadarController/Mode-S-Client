const shell = document.querySelector(".shell");
const progressBar = document.getElementById("progressBar");
const statusLine = document.getElementById("statusLine");
const stages = Array.from(document.querySelectorAll(".stage"));
const readyPill = document.getElementById("readyPill");
const buildText = document.getElementById("buildText");

const fakeStates = [
  { pct: 14, text: "Preparing local services", stage: 0 },
  { pct: 31, text: "Starting HTTP server", stage: 1 },
  { pct: 52, text: "Warming WebView runtime", stage: 2 },
  { pct: 71, text: "Synchronising platform control", stage: 3 },
  { pct: 83, text: "Standing by for UI handover", stage: 3 },
];

let idx = 0;
let holdAt = 83;
let loop = null;

function setStage(index){
  stages.forEach((el, i) => {
    el.classList.toggle("stage--active", i === index);
    el.classList.toggle("stage--done", i < index);
  });
}

function setProgress(pct){
  progressBar.style.width = `${Math.max(0, Math.min(100, pct))}%`;
}

function tickFakeLoad(){
  const state = fakeStates[Math.min(idx, fakeStates.length - 1)];
  setProgress(state.pct);
  statusLine.textContent = state.text;
  setStage(state.stage);
  idx += 1;

  if (idx >= fakeStates.length) {
    clearInterval(loop);
    loop = setInterval(() => {
      holdAt = Math.min(92, holdAt + 1);
      setProgress(holdAt);
    }, 380);
  }
}

window.__setSplashMeta = function(displayName, versionText){
  if (buildText && versionText) buildText.textContent = versionText;
};

window.__onNativeReady = function(){
  if (shell) shell.classList.add("is-ready");
  setProgress(100);
  statusLine.textContent = "Startup complete — handing over to application UI";
  setStage(stages.length);
  if (readyPill) {
    readyPill.textContent = "READY";
    readyPill.classList.add("footer__pill--ready");
  }
};

document.addEventListener("DOMContentLoaded", () => {
  if (buildText && window.__SPLASH_VERSION) buildText.textContent = window.__SPLASH_VERSION;
  tickFakeLoad();
  loop = setInterval(tickFakeLoad, 850);
});
