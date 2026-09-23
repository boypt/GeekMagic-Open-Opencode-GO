function sceneHandler() {
  return {
    current: "",
    loading: false,
    statusMsg: "",

    fetchScene() {
      apiFetch("/api/v1/scene")
        .then((r) => r.json())
        .then((data) => {
          this.current = data.current || "";
        })
        .catch((err) => {
          console.error(err);
        });
    },

    switchScene(name) {
      this.loading = true;
      this.statusMsg = "";
      apiFetch("/api/v1/scene", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ scene: name }),
      })
        .then((r) => r.json())
        .then((data) => {
          if (data.status === "ok") {
            this.current = data.current;
            this.statusMsg = "Scene: " + data.current;
          } else {
            this.statusMsg = data.message || "Scene switch failed";
          }
        })
        .catch((err) => {
          this.statusMsg = "Scene switch failed";
          console.error(err);
        })
        .finally(() => {
          this.loading = false;
        });
    },

    init() {
      this.fetchScene();
    },
  };
}
