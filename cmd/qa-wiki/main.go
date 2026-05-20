package main

import (
	"archive/zip"
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

var version = "1.1.0"

var (
	appSecret string
	dbPath    string
	filesDir  string
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
	filesDir = filepath.Join(filepath.Dir(dbPath), "files")
	os.MkdirAll(filesDir, 0755)

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

	mux.HandleFunc("/api/version", func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, 200, map[string]string{"version": version})
	})

	mux.HandleFunc("/api/auth", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleAuth(w, r)
	})

	mux.HandleFunc("/api/stats", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleStats(w, r)
	})

	mux.HandleFunc("/api/upload", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleUpload(w, r)
	})

	// Serve uploaded files — /files/ canonical; /images/ for existing content
	mux.Handle("/files/", http.StripPrefix("/files/", http.FileServer(http.Dir(filesDir))))
	mux.Handle("/images/", http.StripPrefix("/images/", http.FileServer(http.Dir(filesDir))))

	mux.HandleFunc("/api/qa", func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			handleQASearch(w, r)
		case http.MethodPost:
			handleQACreate(w, r)
		default:
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
		}
	})

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
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
		}
	})

	mux.HandleFunc("/api/db/export", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBExport(w, r)
	})

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

// ----- File upload (admin only, all types) -----

func handleUpload(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}

	if err := r.ParseMultipartForm(50 << 20); err != nil { // 50 MB
		writeJSON(w, 400, map[string]string{"error": "parse error: " + err.Error()})
		return
	}
	uploadedFile, header, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "missing file: " + err.Error()})
		return
	}
	defer uploadedFile.Close()

	ext := strings.ToLower(filepath.Ext(header.Filename))
	name := fmt.Sprintf("%d%s", time.Now().UnixNano(), ext)
	dst, err := os.Create(filepath.Join(filesDir, name))
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "create file: " + err.Error()})
		return
	}
	defer dst.Close()

	if _, err := io.Copy(dst, uploadedFile); err != nil {
		writeJSON(w, 500, map[string]string{"error": "write file: " + err.Error()})
		return
	}

	// Determine if it's an image for markdown syntax
	isImage := ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp" || ext == ".svg" || ext == ".bmp"

	writeJSON(w, 200, map[string]string{
		"url":     "/files/" + name,
		"name":    header.Filename,
		"isImage": fmt.Sprintf("%v", isImage),
	})
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

// ----- DB Export / Import -----

func handleDBExport(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	// Checkpoint WAL so the exported file is complete
	dbMu.Lock()
	database.Exec("PRAGMA wal_checkpoint(TRUNCATE)")
	dbMu.Unlock()

	// Export as zip: data.db + files/ directory
	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", `attachment; filename="qa-wiki-export.zip"`)
	zw := zip.NewWriter(w)
	defer zw.Close()

	// Add data.db
	addFileToZip(zw, dbPath, "data.db")

	// Add all files from filesDir
	filepath.Walk(filesDir, func(path string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(filepath.Dir(filesDir), path)
		addFileToZip(zw, path, rel)
		return nil
	})
}

func addFileToZip(zw *zip.Writer, srcPath, zipName string) {
	f, err := os.Open(srcPath)
	if err != nil {
		return
	}
	defer f.Close()

	w, err := zw.Create(zipName)
	if err != nil {
		return
	}
	io.Copy(w, f)
}

func handleDBImport(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}

	if err := r.ParseMultipartForm(200 << 20); err != nil { // 200 MB
		writeJSON(w, 400, map[string]string{"error": "parse error: " + err.Error()})
		return
	}
	uploadedFile, header, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "missing file: " + err.Error()})
		return
	}
	defer uploadedFile.Close()

	filename := strings.ToLower(header.Filename)

	if strings.HasSuffix(filename, ".zip") {
		handleDBImportZip(w, r, uploadedFile, header.Filename)
	} else {
		handleDBImportRaw(w, r, uploadedFile, header.Filename)
	}
}

func handleDBImportZip(w http.ResponseWriter, r *http.Request, uploadedFile io.Reader, originalName string) {
	// Save zip to temp file so we can read it with archive/zip
	tmpZip, err := os.Create(dbPath + ".import.zip")
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "create temp: " + err.Error()})
		return
	}
	defer os.Remove(tmpZip.Name())

	if _, err := io.Copy(tmpZip, uploadedFile); err != nil {
		tmpZip.Close()
		writeJSON(w, 500, map[string]string{"error": "write temp: " + err.Error()})
		return
	}
	tmpZip.Close()

	zr, err := zip.OpenReader(tmpZip.Name())
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "无效的 zip 文件: " + err.Error()})
		return
	}
	defer zr.Close()

	var dbEntry, filesPrefix string
	for _, f := range zr.File {
		if f.Name == "data.db" {
			dbEntry = f.Name
		}
		if strings.HasPrefix(f.Name, "files/") && filesPrefix == "" {
			filesPrefix = "files/"
		}
	}
	if dbEntry == "" {
		writeJSON(w, 400, map[string]string{"error": "zip 中没有找到 data.db"})
		return
	}

	// Extract data.db for validation
	tmpDBPath := dbPath + ".import"
	if err := extractZipFile(zr, "data.db", tmpDBPath); err != nil {
		writeJSON(w, 400, map[string]string{"error": "提取 data.db 失败: " + err.Error()})
		return
	}
	defer os.Remove(tmpDBPath)

	// Validate
	testDB, err := db.Open(tmpDBPath)
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "无效的数据库文件: " + err.Error()})
		return
	}
	var tableName string
	err = testDB.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='questions'").Scan(&tableName)
	testDB.Close()
	if err != nil || tableName != "questions" {
		writeJSON(w, 400, map[string]string{"error": "数据库不包含 questions 表"})
		return
	}

	// Swap databases
	backupPath := dbPath + ".backup"
	dbMu.Lock()
	oldDB := database
	database = nil
	dbMu.Unlock()
	if oldDB != nil {
		oldDB.Close()
	}

	os.Remove(backupPath)
	os.Rename(dbPath, backupPath)
	if err := os.Rename(tmpDBPath, dbPath); err != nil {
		os.Rename(backupPath, dbPath)
		newDB, _ := db.Open(dbPath)
		dbMu.Lock()
		database = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "替换数据库失败: " + err.Error()})
		return
	}

	// Extract files
	filesExtracted := 0
	for _, f := range zr.File {
		if strings.HasPrefix(f.Name, "files/") && !f.FileInfo().IsDir() {
			destPath := filepath.Join(filesDir, strings.TrimPrefix(f.Name, "files/"))
			os.MkdirAll(filepath.Dir(destPath), 0755)
			if err := extractZipFile(zr, f.Name, destPath); err == nil {
				filesExtracted++
			}
		}
	}

	newDB, err := db.Open(dbPath)
	if err != nil {
		os.Remove(dbPath)
		os.Rename(backupPath, dbPath)
		newDB, _ = db.Open(dbPath)
		dbMu.Lock()
		database = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "打开数据库失败: " + err.Error()})
		return
	}

	dbMu.Lock()
	database = newDB
	dbMu.Unlock()

	writeJSON(w, 200, map[string]string{
		"ok":             "imported",
		"name":           originalName,
		"backup":         backupPath,
		"filesExtracted": fmt.Sprintf("%d", filesExtracted),
	})
}

func handleDBImportRaw(w http.ResponseWriter, r *http.Request, uploadedFile io.Reader, originalName string) {
	tmpPath := dbPath + ".import"
	dst, err := os.Create(tmpPath)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "create temp: " + err.Error()})
		return
	}
	if _, err := io.Copy(dst, uploadedFile); err != nil {
		dst.Close()
		os.Remove(tmpPath)
		writeJSON(w, 500, map[string]string{"error": "write temp: " + err.Error()})
		return
	}
	dst.Close()
	defer os.Remove(tmpPath)

	testDB, err := db.Open(tmpPath)
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "无效的数据库文件: " + err.Error()})
		return
	}
	var tableName string
	err = testDB.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='questions'").Scan(&tableName)
	testDB.Close()
	if err != nil || tableName != "questions" {
		writeJSON(w, 400, map[string]string{"error": "数据库不包含 questions 表"})
		return
	}

	backupPath := dbPath + ".backup"
	dbMu.Lock()
	oldDB := database
	database = nil
	dbMu.Unlock()
	if oldDB != nil {
		oldDB.Close()
	}

	os.Remove(backupPath)
	os.Rename(dbPath, backupPath)
	if err := os.Rename(tmpPath, dbPath); err != nil {
		os.Rename(backupPath, dbPath)
		newDB, _ := db.Open(dbPath)
		dbMu.Lock()
		database = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "替换数据库失败: " + err.Error()})
		return
	}

	newDB, err := db.Open(dbPath)
	if err != nil {
		os.Remove(dbPath)
		os.Rename(backupPath, dbPath)
		newDB, _ = db.Open(dbPath)
		dbMu.Lock()
		database = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "打开数据库失败: " + err.Error()})
		return
	}

	dbMu.Lock()
	database = newDB
	dbMu.Unlock()

	writeJSON(w, 200, map[string]string{
		"ok":     "imported",
		"name":   originalName,
		"backup": backupPath,
	})
}

func extractZipFile(zr *zip.ReadCloser, name, destPath string) error {
	for _, f := range zr.File {
		if f.Name == name {
			rc, err := f.Open()
			if err != nil {
				return err
			}
			defer rc.Close()

			dst, err := os.Create(destPath)
			if err != nil {
				return err
			}
			defer dst.Close()

			_, err = io.Copy(dst, rc)
			return err
		}
	}
	return fmt.Errorf("file %s not found in zip", name)
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
