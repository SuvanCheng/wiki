package web

import "embed"

//go:embed lib index.html style.css app.js
var FS embed.FS
