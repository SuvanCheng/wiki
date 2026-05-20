package main

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"qa-wiki/internal/db"
	"qa-wiki/web"
)

// Set via -ldflags at build time, e.g. -X main.version=1.2.3
var version = "dev"

var (
	appSecret string
	dbPath    string
	imagesDir string
	dbMu      sync.Mutex
	database  *sql.DB
)

func main() {
	appSecret = os.Getenv("QA_SECRET")
	if appSecret == "" {
		appSecret = "hygon123;"
	}
	fmt.Printf("QA Wiki v%s  管理密钥: %s\n", version, appSecret)

	dbPath = resolveDBPath()
	imagesDir = filepath.Join(filepath.Dir(dbPath), "images")
	os.MkdirAll(imagesDir, 0755)

	var err error
	database, err = db.Open(dbPath)
	if err != nil {
		log.Fatalf("打开数据库失败: %v", err)
	}
	defer database.Close()

	if err := db.InitSchema(database); err != nil {
		log.Fatalf("初始化数据库失败: %v", err)
	}

	count, err := db.Count(database)
	if err != nil {
		log.Fatalf("查询数据库失败: %v", err)
	}
	if count == 0 {
		if err := db.InsertMockData(database); err != nil {
			log.Fatalf("写入 Mock 数据失败: %v", err)
		}
		fmt.Println("已写入 3 条 Mock 数据")
	}

	mux := http.NewServeMux()

	// /api/version
	mux.HandleFunc("/api/version", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]string{"version": version})
	})

	// /api/auth
	mux.HandleFunc("/api/auth", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleAuth(w, r)
	})

	// /api/stats
	mux.HandleFunc("/api/stats", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleStats(w, r)
	})

	// /api/upload (admin only)
	mux.HandleFunc("/api/upload", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleUpload(w, r)
	})

	// /images/
	mux.Handle("/images/", http.StripPrefix("/images/", http.FileServer(http.Dir(imagesDir))))

	// /api/qa — search (GET, public) and create (POST, admin)
	mux.HandleFunc("/api/qa", func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			handleQASearch(w, r)
		case http.MethodPost:
			handleQACreate(w, r)
		default:
			w.Header().Set("Allow", "GET, POST")
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
		}
	})

	// /api/qa/{id}
	mux.HandleFunc("/api/qa/", func(w http.ResponseWriter, r *http.Request) {
		idStr := strings.TrimPrefix(r.URL.Path, "/api/qa/")
		id, err := strconv.Atoi(idStr)
		if err != nil || id <= 0 {
			http.Error(w, "invalid id", http.StatusBadRequest)
			return
		}
		switch r.Method {
		case http.MethodGet:
			handleQAGetByID(w, r, id)
		case http.MethodPut:
			handleQAUpdate(w, r, id)
		case http.MethodDelete:
			handleQADelete(w, r, id)
		default:
			w.Header().Set("Allow", "GET, PUT, DELETE")
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
		}
	})

	// /api/db/export (admin only)
	mux.HandleFunc("/api/db/export", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBExport(w, r)
	})

	// /api/db/import (admin only)
	mux.HandleFunc("/api/db/import", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBImport(w, r)
	})

	mux.Handle("/", http.FileServer(http.FS(web.FS)))

	addr := "127.0.0.1:8080"
	url := "http://" + addr

	go func() {
		if err := openBrowser(url); err != nil {
			fmt.Printf("无法自动打开浏览器，请手动访问 %s\n", url)
		}
	}()

	fmt.Printf("QA Wiki 已启动: %s\n", url)
	fmt.Println("按 Ctrl+C 退出")
	log.Fatal(http.ListenAndServe(addr, mux))
}

func resolveDBPath() string {
	execPath, err := os.Executable()
	if err == nil {
		dir := filepath.Dir(execPath)
		if !strings.Contains(dir, os.TempDir()) && !strings.Contains(dir, "go-build") {
			return filepath.Join(dir, "data.db")
		}
	}
	return "data.db"
}

func isAuthenticated(r *http.Request) bool {
	return r.Header.Get("X-Auth") == appSecret
}

func requireAuth(w http.ResponseWriter, r *http.Request) bool {
	if !isAuthenticated(r) {
		writeJSON(w, 403, map[string]string{"error": "需要管理员权限"})
		return false
	}
	return true
}

func writeJSON(w http.ResponseWriter, status int, v interface{}) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

// ----- Auth -----

func handleAuth(w http.ResponseWriter, r *http.Request) {
	var body struct{ Secret string `json:"secret"` }
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid JSON"})
		return
	}
	if body.Secret != appSecret {
		writeJSON(w, 403, map[string]string{"error": "密钥错误"})
		return
	}
	writeJSON(w, 200, map[string]string{"ok": "authenticated"})
}

// ----- Stats -----

func handleStats(w http.ResponseWriter, r *http.Request) {
	stats, err := db.GetStats(database, isAuthenticated(r))
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, stats)
}

// ----- Image upload (admin only) -----

func handleUpload(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}

	if err := r.ParseMultipartForm(20 << 20); err != nil {
		writeJSON(w, 400, map[string]string{"error": "parse error: " + err.Error()})
		return
	}
	file, header, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "missing file: " + err.Error()})
		return
	}
	defer file.Close()

	ext := strings.ToLower(filepath.Ext(header.Filename))
	if ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".gif" && ext != ".webp" && ext != ".svg" && ext != ".bmp" {
		writeJSON(w, 400, map[string]string{"error": "unsupported image type: " + ext})
		return
	}

	name := fmt.Sprintf("%d%s", time.Now().UnixNano(), ext)
	dst, err := os.Create(filepath.Join(imagesDir, name))
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "create file: " + err.Error()})
		return
	}
	defer dst.Close()

	if _, err := io.Copy(dst, file); err != nil {
		writeJSON(w, 500, map[string]string{"error": "write file: " + err.Error()})
		return
	}
	writeJSON(w, 200, map[string]string{"url": "/images/" + name, "name": header.Filename})
}

// ----- QA handlers -----

func handleQASearch(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("q")
	results, err := db.Search(database, q, isAuthenticated(r))
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, results)
}

func handleQACreate(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	var entry db.QA
	if err := json.NewDecoder(r.Body).Decode(&entry); err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid JSON: " + err.Error()})
		return
	}
	if strings.TrimSpace(entry.Question) == "" {
		writeJSON(w, 400, map[string]string{"error": "question is required"})
		return
	}
	if entry.Visibility != "internal" && entry.Visibility != "public" {
		entry.Visibility = "internal"
	}
	if err := db.Insert(database, &entry); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 201, entry)
}

func handleQAGetByID(w http.ResponseWriter, r *http.Request, id int) {
	entry, err := db.GetByID(database, id)
	if err != nil {
		writeJSON(w, 404, map[string]string{"error": "not found"})
		return
	}
	writeJSON(w, 200, entry)
}

func handleQAUpdate(w http.ResponseWriter, r *http.Request, id int) {
	if !requireAuth(w, r) {
		return
	}
	var entry db.QA
	if err := json.NewDecoder(r.Body).Decode(&entry); err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid JSON: " + err.Error()})
		return
	}
	entry.ID = id
	if strings.TrimSpace(entry.Question) == "" {
		writeJSON(w, 400, map[string]string{"error": "question is required"})
		return
	}
	if entry.Visibility != "internal" && entry.Visibility != "public" {
		entry.Visibility = "internal"
	}
	if err := db.Update(database, &entry); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, entry)
}

func handleQADelete(w http.ResponseWriter, r *http.Request, id int) {
	if !requireAuth(w, r) {
		return
	}
	if err := db.Delete(database, id); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, map[string]string{"ok": "deleted"})
}

// ----- DB Export / Import (admin only) -----

func handleDBExport(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	// Force WAL checkpoint so all data is flushed to the main DB file
	dbMu.Lock()
	database.Exec("PRAGMA wal_checkpoint(TRUNCATE)")
	dbMu.Unlock()

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="data.db"`)
	http.ServeFile(w, r, dbPath)
}

func handleDBImport(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}

	if err := r.ParseMultipartForm(100 << 20); err != nil { // 100 MB
		writeJSON(w, 400, map[string]string{"error": "parse error: " + err.Error()})
		return
	}
	file, header, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "missing file: " + err.Error()})
		return
	}
	defer file.Close()

	// Write to temp file for validation
	tmpPath := dbPath + ".import"
	dst, err := os.Create(tmpPath)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "create temp: " + err.Error()})
		return
	}
	if _, err := io.Copy(dst, file); err != nil {
		dst.Close()
		os.Remove(tmpPath)
		writeJSON(w, 500, map[string]string{"error": "write temp: " + err.Error()})
		return
	}
	dst.Close()

	// Validate: must be a valid SQLite DB with questions table
	testDB, err := db.Open(tmpPath)
	if err != nil {
		os.Remove(tmpPath)
		writeJSON(w, 400, map[string]string{"error": "无效的数据库文件: " + err.Error()})
		return
	}
	var tableName string
	err = testDB.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='questions'").Scan(&tableName)
	testDB.Close()
	if err != nil || tableName != "questions" {
		os.Remove(tmpPath)
		writeJSON(w, 400, map[string]string{"error": "数据库不包含 questions 表，格式不正确"})
		return
	}

	// Backup old DB and swap
	backupPath := dbPath + ".backup"
	dbMu.Lock()
	oldDB := database
	database = nil
	dbMu.Unlock()

	if oldDB != nil {
		oldDB.Close()
	}

	// Rename current to backup, import to current
	os.Remove(backupPath)
	os.Rename(dbPath, backupPath)
	if err := os.Rename(tmpPath, dbPath); err != nil {
		// Restore from backup
		os.Rename(backupPath, dbPath)
		os.Remove(tmpPath)
		// Reopen
		newDB, err := db.Open(dbPath)
		if err != nil {
			log.Fatalf("恢复数据库失败: %v", err)
		}
		dbMu.Lock()
		database = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "替换数据库文件失败: " + err.Error()})
		return
	}

	// Open new database
	newDB, err := db.Open(dbPath)
	if err != nil {
		// Restore from backup
		os.Remove(dbPath)
		os.Rename(backupPath, dbPath)
		newDB, _ = db.Open(dbPath)
		dbMu.Lock()
		database = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "打开发布数据库失败: " + err.Error()})
		return
	}

	dbMu.Lock()
	database = newDB
	dbMu.Unlock()

	writeJSON(w, 200, map[string]string{
		"ok":     "imported",
		"name":   header.Filename,
		"backup": backupPath,
	})
}

// ----- Browser -----

func openBrowser(url string) error {
	var cmd *exec.Cmd
	switch runtime.GOOS {
	case "darwin":
		cmd = exec.Command("open", url)
	case "windows":
		cmd = exec.Command("cmd", "/c", "start", url)
	default:
		cmd = exec.Command("xdg-open", url)
	}
	return cmd.Start()
}
