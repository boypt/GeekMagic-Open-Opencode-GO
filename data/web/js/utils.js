function humanFileSize(bytes) {
  if (bytes === 0) {
    return "0 B";
  }

  const k = 1024;
  const sizes = ["B", "KB", "MB"];
  const i = Math.floor(Math.log(bytes) / Math.log(k));

  return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + " " + sizes[i];
}

// 默认 API token：与文件系统 data/config.json 的 api_token 保持一致。
// 浏览器 localStorage 里没有 token 时自动使用该默认值，免去手动到 Token 页配置。
const DEFAULT_API_TOKEN = "geekmagic2026";

function apiFetch(url, options = {}) {
  const stored = localStorage.getItem("Authorization");
  const token = stored || DEFAULT_API_TOKEN;

  const fetchOptions = { ...options };

  if (!fetchOptions.headers) {
    fetchOptions.headers = {};
  } else {
    fetchOptions.headers = { ...fetchOptions.headers };
  }

  if (token) {
    fetchOptions.headers["Authorization"] = `Bearer ${token}`;
  }

  return fetch(url, fetchOptions).then((res) => {
    // 本地缓存的 token 可能已过期（例如设备 token 被 config.json 覆盖）。
    // 遇到 401 时用内置默认 token 重试一次，成功则更新本地缓存，免去手动改配置。
    if (res.status === 401 && token !== DEFAULT_API_TOKEN && DEFAULT_API_TOKEN) {
      return fetch(url, {
        ...fetchOptions,
        headers: {
          ...fetchOptions.headers,
          Authorization: `Bearer ${DEFAULT_API_TOKEN}`,
        },
      }).then((retryRes) => {
        if (retryRes.ok) {
          localStorage.setItem("Authorization", DEFAULT_API_TOKEN);
        }
        return retryRes;
      });
    }
    return res;
  });
}

function includeHTML(id, url, callback) {
  fetch(url)
    .then((response) => response.text())
    .then((data) => {
      document.getElementById(id).innerHTML = data;
      if (typeof callback === "function") callback();
    });
}

function setHeaderTitle(title) {
  const interval = setInterval(() => {
    const h1 = document.getElementById("header-title");
    if (h1) {
      h1.textContent = title;
      clearInterval(interval);
    }
  }, 20);
}

document.addEventListener("DOMContentLoaded", () => {
  if (document.getElementById("header-placeholder")) {
    includeHTML("header-placeholder", "./header.html", () => {
      let pageTitle =
        document.title && document.title.trim()
          ? document.title.trim()
          : "Placeholder Title";
      setHeaderTitle(pageTitle);
    });
  }
  if (document.getElementById("footer-placeholder")) {
    includeHTML("footer-placeholder", "./footer.html");
  }
});
