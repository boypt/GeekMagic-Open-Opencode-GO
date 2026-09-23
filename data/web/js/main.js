document.addEventListener("alpine:init", () => {
  Alpine.data("themeSwitcher", themeSwitcher);
  if (typeof otaUploadHandler !== "undefined")
    Alpine.data("otaUploadHandler", otaUploadHandler);
  if (typeof albumUploadHandler !== "undefined")
    Alpine.data("albumUploadHandler", albumUploadHandler);
  if (typeof wifiHandler !== "undefined")
    Alpine.data("wifiHandler", wifiHandler);
  if (typeof ntpHandler !== "undefined") Alpine.data("ntpHandler", ntpHandler);
  if (typeof rotationHandler !== "undefined")
    Alpine.data("rotationHandler", rotationHandler);
  if (typeof sceneHandler !== "undefined")
    Alpine.data("sceneHandler", sceneHandler);
  if (typeof lightHandler !== "undefined")
    Alpine.data("lightHandler", lightHandler);
  if (typeof rebootHandler !== "undefined")
    Alpine.data("rebootHandler", rebootHandler);
  if (typeof tokenHandler !== "undefined")
    Alpine.data("tokenHandler", tokenHandler);
});
