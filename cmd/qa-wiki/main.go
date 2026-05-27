package main

import (
	"archive/zip"
	"context"
	"database/sql"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"math/rand"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"os/signal"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"qa-wiki/internal/db"
	"qa-wiki/web"
)

var version = "1.10.0"

var (
	appSecret    string
	dbPath       string
	filesDir     string
	dbMu         sync.RWMutex
	dbConns      = make(map[string]*sql.DB)
	registryPath string
	dbRegistry   []db.DBEntry
	activeDBPath string
	verbose      bool
)

var dbColors = []string{
	"#4a9eff", "#e07b5a", "#5ac8a0", "#ff6b6b",
	"#ffd93d", "#6c5ce7", "#a29bfe", "#fd79a8",
}

var fileRefRe = regexp.MustCompile(`/(?:files|images)/([^\s)"'<>]+)`)

func main() {
	flag.BoolVar(&verbose, "v", false, "详细调试输出")
	flag.Parse()

	appSecret = os.Getenv("QA_SECRET")
	if appSecret == "" {
		appSecret = "hygon123;"
	}
	dbPath = resolveDBPath()
	registryPath = filepath.Join(filepath.Dir(dbPath), "databases.json")
	filesDir = filepath.Join(filepath.Dir(dbPath), "files")
	os.MkdirAll(filesDir, 0755)

	loadRegistry()
	openAllDBs()

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
		path := strings.TrimPrefix(r.URL.Path, "/api/qa/")

		if strings.HasSuffix(path, "/star") {
			idStr := strings.TrimSuffix(path, "/star")
			id, err := strconv.Atoi(idStr)
			if err != nil || id <= 0 {
				http.Error(w, "invalid id", http.StatusBadRequest)
				return
			}
			if r.Method != http.MethodPost {
				http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
				return
			}
			handleQAToggleStar(w, r, id)
			return
		}

		idStr := path
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

	mux.HandleFunc("/api/db/list", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBList(w, r)
	})

	mux.HandleFunc("/api/db/name", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPut {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBRename(w, r)
	})

	mux.HandleFunc("/api/db/load", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBLoad(w, r)
	})

	mux.HandleFunc("/api/db/remove", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBRemove(w, r)
	})

	mux.HandleFunc("/api/db/switch", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "405 method not allowed", http.StatusMethodNotAllowed)
			return
		}
		handleDBSwitch(w, r)
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

	addr := "0.0.0.0:11799"
	if envAddr := os.Getenv("QA_ADDR"); envAddr != "" {
		if !strings.Contains(envAddr, ":") {
			envAddr = "0.0.0.0:" + envAddr
		}
		addr = envAddr
	}
	_, port, _ := net.SplitHostPort(addr)
	if port == "" {
		port = "11799"
	}
	localURL := "http://127.0.0.1:" + port
	fmt.Printf("QA Wiki %s  管理密钥: %s\n", version, appSecret)
	fmt.Printf("本地访问: %s\n", localURL)

	if verbose {
		debugf("监听地址: %s, 端口: %s", addr, port)
		debugf("数据库路径: %s", dbPath)
		debugf("文件存储: %s", filesDir)
		listInterfaces()
	}

	fmt.Printf("局域网访问: http://<服务器IP>:%s\n", port)
	fmt.Println("按 Ctrl+C 退出")

	go func() {
		if err := openBrowser(localURL); err != nil {
			fmt.Printf("无法自动打开浏览器，请手动访问 %s\n", localURL)
		}
	}()

	var handler http.Handler = mux
	if verbose {
		handler = loggingMiddleware(mux)
	}

	ln, err := listen(addr)
	if err != nil {
		log.Fatalf("监听失败: %v", err)
	}
	debugf("服务已启动: %s", ln.Addr())

	idle := &http.Server{Handler: handler}
	go func() {
		sigCh := make(chan os.Signal, 1)
		signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
		sig := <-sigCh
		debugf("收到信号 %v，正在关闭...", sig)
		idle.Shutdown(context.Background())
	}()

	if err := idle.Serve(ln); err != http.ErrServerClosed {
		log.Fatal(err)
	}
	fmt.Println("服务已关闭")
}

// ----- Registry management -----

func loadRegistry() {
	data, err := os.ReadFile(registryPath)
	if err != nil {
		initDefaultRegistry()
		return
	}
	if err := json.Unmarshal(data, &dbRegistry); err != nil || len(dbRegistry) == 0 {
		initDefaultRegistry()
		return
	}
	usedColors := make(map[string]bool)
	for _, e := range dbRegistry {
		if e.Color != "" {
			usedColors[e.Color] = true
		}
	}
	for i := range dbRegistry {
		if dbRegistry[i].Color == "" {
			dbRegistry[i].Color = pickColor(usedColors)
			usedColors[dbRegistry[i].Color] = true
		}
	}
	activeDBPath = dbRegistry[0].Path
}

func initDefaultRegistry() {
	absPath, _ := filepath.Abs(dbPath)
	name := "默认知识库"
	if d, err := db.Open(dbPath); err == nil {
		if n, err := db.GetMeta(d, "name"); err == nil && n != "" {
			name = n
		}
		db.SetMeta(d, "name", name)
		d.Close()
	}
	dbRegistry = []db.DBEntry{{Name: name, Path: absPath, Color: dbColors[0], Editable: true}}
	activeDBPath = absPath
	saveRegistry()
}

func saveRegistry() {
	data, _ := json.MarshalIndent(dbRegistry, "", "  ")
	os.WriteFile(registryPath, data, 0644)
}

func pickColor(used map[string]bool) string {
	for _, c := range dbColors {
		if !used[c] {
			return c
		}
	}
	return fmt.Sprintf("#%06x", rand.Intn(0xFFFFFF))
}

func openAllDBs() {
	for i := range dbRegistry {
		entry := &dbRegistry[i]
		conn, err := db.Open(entry.Path)
		if err != nil {
			fmt.Printf("警告: 无法打开数据库 %s: %v\n", entry.Name, err)
			continue
		}
		db.InitSchema(conn)
		dbConns[entry.Path] = conn

		var count int
		conn.QueryRow("SELECT COUNT(*) FROM questions").Scan(&count)
		if count == 0 && entry.Path == dbPath {
			db.InsertMockData(conn)
			fmt.Println("已写入 3 条 Mock 数据")
		}

		if n, _ := db.GetMeta(conn, "name"); n != "" && entry.Name != n {
			entry.Name = n
		}
	}
}

func activeDB() *sql.DB {
	dbMu.RLock()
	defer dbMu.RUnlock()
	if conn, ok := dbConns[activeDBPath]; ok {
		return conn
	}
	return nil
}

func resolveDB(r *http.Request) (*sql.DB, string, string) {
	dbParam := r.URL.Query().Get("db")
	if dbParam == "*" {
		return nil, "*", ""
	}
	if dbParam != "" {
		dbMu.RLock()
		defer dbMu.RUnlock()
		for _, entry := range dbRegistry {
			if entry.Name == dbParam {
				if conn, ok := dbConns[entry.Path]; ok {
					return conn, entry.Name, entry.Path
				}
			}
		}
	}
	d := activeDB()
	name := ""
	path := ""
	dbMu.RLock()
	for _, entry := range dbRegistry {
		if entry.Path == activeDBPath {
			name = entry.Name
			path = entry.Path
			break
		}
	}
	dbMu.RUnlock()
	return d, name, path
}

func dbByPath(path string) *sql.DB {
	dbMu.RLock()
	defer dbMu.RUnlock()
	return dbConns[path]
}

// ----- Helpers -----

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

func expandPath(path string) string {
	if strings.HasPrefix(path, "~/") {
		home, err := os.UserHomeDir()
		if err == nil {
			return filepath.Join(home, path[2:])
		}
	} else if path == "~" {
		home, err := os.UserHomeDir()
		if err == nil {
			return home
		}
	}
	return path
}

func safeDBPath(originalName string) string {
	dir := filepath.Dir(dbPath)
	name := filepath.Base(originalName)
	if !strings.HasSuffix(strings.ToLower(name), ".db") {
		ext := filepath.Ext(name)
		name = strings.TrimSuffix(name, ext) + ".db"
	}
	dest := filepath.Join(dir, name)
	if _, err := os.Stat(dest); os.IsNotExist(err) {
		return dest
	}
	base := strings.TrimSuffix(name, ".db")
	for i := 1; ; i++ {
		alt := filepath.Join(dir, fmt.Sprintf("%s_%d.db", base, i))
		if _, err := os.Stat(alt); os.IsNotExist(err) {
			return alt
		}
	}
}

func registerNewDB(destPath, displayName string, w http.ResponseWriter) {
	newDB, err := db.Open(destPath)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "打开数据库失败: " + err.Error()})
		return
	}
	db.InitSchema(newDB)

	name := displayName
	if name == "" {
		name = strings.TrimSuffix(filepath.Base(destPath), ".db")
	}
	if n, _ := db.GetMeta(newDB, "name"); n != "" && n != name {
		name = n
	}
	db.SetMeta(newDB, "name", name)

	dbMu.Lock()
	usedColors := make(map[string]bool)
	for _, e := range dbRegistry {
		usedColors[e.Color] = true
	}
	color := pickColor(usedColors)
	dbConns[destPath] = newDB
	dbRegistry = append(dbRegistry, db.DBEntry{Name: name, Path: destPath, Color: color, Editable: true})
	activeDBPath = destPath
	saveRegistry()
	dbMu.Unlock()

	var count int
	newDB.QueryRow("SELECT COUNT(*) FROM questions").Scan(&count)

	writeJSON(w, 200, map[string]interface{}{
		"ok":    "loaded_new",
		"name":  name,
		"path":  destPath,
		"color": color,
		"count": count,
	})
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

func debugf(format string, args ...interface{}) {
	if verbose {
		log.Printf("[DEBUG] "+format, args...)
	}
}

func listInterfaces() {
	ifaces, err := net.Interfaces()
	if err != nil {
		debugf("获取网络接口失败: %v", err)
		return
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, a := range addrs {
			ipnet, ok := a.(*net.IPNet)
			if !ok {
				continue
			}
			if ipnet.IP.IsLoopback() {
				continue
			}
			if ip4 := ipnet.IP.To4(); ip4 != nil {
				debugf("网卡 %s (%s): %s", iface.Name, iface.HardwareAddr, ip4.String())
			}
		}
	}
}

func loggingMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		debugf("=> %s %s 来源: %s", r.Method, r.URL.RequestURI(), r.RemoteAddr)
		next.ServeHTTP(w, r)
		debugf("<= %s %s 耗时: %v", r.Method, r.URL.RequestURI(), time.Since(start))
	})
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
	d, dbName, _ := resolveDB(r)
	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database"})
		return
	}

	dbParam := r.URL.Query().Get("db")
	if dbParam == "*" {
		// Aggregate stats across all databases
		aggregate := db.Stats{
			Categories: []db.CatCount{},
			Recent:     []db.QA{},
		}
		catMap := make(map[string]int)
		var allRecent []db.QA

		dbMu.RLock()
		for path, conn := range dbConns {
			var entryName string
			for _, e := range dbRegistry {
				if e.Path == path {
					entryName = e.Name
					break
				}
			}
			s, err := db.GetStats(conn, isAuthenticated(r))
			if err != nil {
				continue
			}
			aggregate.Total += s.Total
			aggregate.PublicCount += s.PublicCount
			aggregate.InternalCount += s.InternalCount
			aggregate.StarredCount += s.StarredCount
			for _, c := range s.Categories {
				catMap[c.Name] += c.Count
			}
			for i := range s.Recent {
				s.Recent[i].DBSource = entryName
				allRecent = append(allRecent, s.Recent[i])
			}
		}
		dbMu.RUnlock()

		// Sort categories
		type kv struct {
			k string
			v int
		}
		var pairs []kv
		for k, v := range catMap {
			pairs = append(pairs, kv{k, v})
		}
		for i := 0; i < len(pairs); i++ {
			for j := i + 1; j < len(pairs); j++ {
				if pairs[j].v > pairs[i].v || (pairs[j].v == pairs[i].v && pairs[j].k < pairs[i].k) {
					pairs[i], pairs[j] = pairs[j], pairs[i]
				}
			}
		}
		limit := 100
		if len(pairs) < limit {
			limit = len(pairs)
		}
		for i := 0; i < limit; i++ {
			aggregate.Categories = append(aggregate.Categories, db.CatCount{Name: pairs[i].k, Count: pairs[i].v})
		}

		// Sort recent by updated_at desc, take top 5
		sort.Slice(allRecent, func(i, j int) bool {
			return allRecent[i].UpdatedAt > allRecent[j].UpdatedAt
		})
		recentLimit := 5
		if len(allRecent) < recentLimit {
			recentLimit = len(allRecent)
		}
		aggregate.Recent = allRecent[:recentLimit]

		writeJSON(w, 200, aggregate)
		return
	}

	_ = dbName
	stats, err := db.GetStats(d, isAuthenticated(r))
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, stats)
}

// ----- File upload -----

func handleUpload(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}

	if err := r.ParseMultipartForm(50 << 20); err != nil {
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
	useRegex := r.URL.Query().Get("regex") == "true"
	starredOnly := r.URL.Query().Get("starred") == "true"
	authenticated := isAuthenticated(r)

	d, dbName, _ := resolveDB(r)
	dbParam := r.URL.Query().Get("db")

	if dbParam == "*" {
		handleQASearchAll(w, r, q, useRegex, starredOnly, authenticated)
		return
	}

	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database selected"})
		return
	}
	_ = dbName

	if useRegex && q != "" {
		re, err := regexp.Compile(q)
		if err != nil {
			writeJSON(w, 400, map[string]string{"error": "无效的正则表达式: " + err.Error()})
			return
		}
		results, err := db.Search(d, "", authenticated)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": err.Error()})
			return
		}
		var filtered []db.QA
		for _, r := range results {
			if re.MatchString(r.Question) || re.MatchString(r.Answer) || re.MatchString(r.Category) || re.MatchString(r.Author) {
				filtered = append(filtered, r)
			}
		}
		if filtered == nil {
			filtered = []db.QA{}
		}
		writeJSON(w, 200, filtered)
		return
	}

	results, err := db.SearchWithOptions(d, q, authenticated, starredOnly)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, results)
}

func handleQASearchAll(w http.ResponseWriter, r *http.Request, q string, useRegex bool, starredOnly bool, authenticated bool) {
	var allResults []db.QA
	var re *regexp.Regexp
	if useRegex && q != "" {
		re, _ = regexp.Compile(q)
	}

	dbMu.RLock()
	for path, conn := range dbConns {
		var entryName string
		for _, e := range dbRegistry {
			if e.Path == path {
				entryName = e.Name
				break
			}
		}
		results, err := db.SearchWithOptions(conn, "", authenticated, starredOnly)
		if err != nil {
			continue
		}
		for i := range results {
			results[i].DBSource = entryName
			if useRegex && re != nil {
				if re.MatchString(results[i].Question) || re.MatchString(results[i].Answer) || re.MatchString(results[i].Category) || re.MatchString(results[i].Author) {
					allResults = append(allResults, results[i])
				}
			} else if q != "" {
				ql := strings.ToLower(q)
				if strings.Contains(strings.ToLower(results[i].Question), ql) ||
					strings.Contains(strings.ToLower(results[i].Answer), ql) ||
					strings.Contains(strings.ToLower(results[i].Category), ql) ||
					strings.Contains(strings.ToLower(results[i].Author), ql) {
					allResults = append(allResults, results[i])
				}
			} else {
				allResults = append(allResults, results[i])
			}
		}
	}
	dbMu.RUnlock()

	// Sort by updated_at desc
	sort.Slice(allResults, func(i, j int) bool {
		return allResults[i].UpdatedAt > allResults[j].UpdatedAt
	})

	if allResults == nil {
		allResults = []db.QA{}
	}
	writeJSON(w, 200, allResults)
}

func handleQACreate(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	d, _, _ := resolveDB(r)
	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database selected"})
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
	if err := db.Insert(d, &entry); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 201, entry)
}

func handleQAGetByID(w http.ResponseWriter, r *http.Request, id int) {
	d, _, _ := resolveDB(r)
	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database selected"})
		return
	}
	entry, err := db.GetByID(d, id)
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
	d, _, _ := resolveDB(r)
	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database selected"})
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
	if err := db.Update(d, &entry); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, entry)
}

func handleQADelete(w http.ResponseWriter, r *http.Request, id int) {
	if !requireAuth(w, r) {
		return
	}
	d, _, _ := resolveDB(r)
	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database selected"})
		return
	}
	if err := db.Delete(d, id); err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	cleanupOrphanedFiles()
	writeJSON(w, 200, map[string]string{"ok": "deleted"})
}

func handleQAToggleStar(w http.ResponseWriter, r *http.Request, id int) {
	if !requireAuth(w, r) {
		return
	}
	d, _, _ := resolveDB(r)
	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database selected"})
		return
	}
	starred, err := db.ToggleStar(d, id)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": err.Error()})
		return
	}
	writeJSON(w, 200, map[string]interface{}{"starred": starred})
}

// ----- DB list / switch / rename / load / remove -----

func handleDBList(w http.ResponseWriter, r *http.Request) {
	dbMu.RLock()
	defer dbMu.RUnlock()

	type DBInfo struct {
		Name  string `json:"name"`
		Path  string `json:"path"`
		Color string `json:"color"`
		Count int    `json:"count"`
	}
	var list []DBInfo
	for _, entry := range dbRegistry {
		count := 0
		if conn, ok := dbConns[entry.Path]; ok {
			conn.QueryRow("SELECT COUNT(*) FROM questions").Scan(&count)
		}
		list = append(list, DBInfo{
			Name:  entry.Name,
			Path:  entry.Path,
			Color: entry.Color,
			Count: count,
		})
	}
	writeJSON(w, 200, list)
}

func handleDBSwitch(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	var body struct{ Name string `json:"name"` }
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid JSON"})
		return
	}

	dbMu.Lock()
	defer dbMu.Unlock()

	for _, entry := range dbRegistry {
		if entry.Name == body.Name {
			if _, ok := dbConns[entry.Path]; ok {
				activeDBPath = entry.Path
				writeJSON(w, 200, map[string]string{"ok": "switched", "name": entry.Name, "path": entry.Path})
				return
			}
		}
	}
	writeJSON(w, 404, map[string]string{"error": "database not found"})
}

func handleDBRename(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	var body struct {
		Path    string `json:"path"`
		NewName string `json:"newName"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid JSON"})
		return
	}
	if strings.TrimSpace(body.NewName) == "" {
		writeJSON(w, 400, map[string]string{"error": "name is required"})
		return
	}

	dbMu.Lock()
	defer dbMu.Unlock()

	// Check for name conflict
	for _, entry := range dbRegistry {
		if entry.Name == body.NewName && entry.Path != body.Path {
			writeJSON(w, 409, map[string]string{"error": "名称已存在: " + body.NewName})
			return
		}
	}

	for i := range dbRegistry {
		if dbRegistry[i].Path == body.Path {
			dbRegistry[i].Name = body.NewName
			if conn, ok := dbConns[body.Path]; ok {
				db.SetMeta(conn, "name", body.NewName)
			}
			saveRegistry()
			writeJSON(w, 200, map[string]string{"ok": "renamed"})
			return
		}
	}
	writeJSON(w, 404, map[string]string{"error": "database not found"})
}

func handleDBLoad(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	var body struct {
		Path       string `json:"path"`
		Name       string `json:"name"`
		OnConflict string `json:"onConflict"` // "rename", "replace", "skip"
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid JSON"})
		return
	}
	if body.Path == "" {
		writeJSON(w, 400, map[string]string{"error": "path is required"})
		return
	}

	absPath, err := filepath.Abs(expandPath(body.Path))
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid path"})
		return
	}

	// Check file exists
	if _, err := os.Stat(absPath); os.IsNotExist(err) {
		writeJSON(w, 400, map[string]string{"error": "文件不存在: " + absPath})
		return
	}

	// Open and validate
	testDB, err := db.Open(absPath)
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "无法打开数据库: " + err.Error()})
		return
	}
	var tableName string
	err = testDB.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='questions'").Scan(&tableName)
	if err != nil || tableName != "questions" {
		testDB.Close()
		writeJSON(w, 400, map[string]string{"error": "数据库不包含 questions 表"})
		return
	}
	db.InitSchema(testDB)

	// Determine name
	name := body.Name
	if name == "" {
		if n, err := db.GetMeta(testDB, "name"); err == nil && n != "" {
			name = n
		} else {
			name = filepath.Base(absPath)
			name = strings.TrimSuffix(name, filepath.Ext(name))
		}
	}

	dbMu.Lock()
	defer dbMu.Unlock()

	// Check for conflicts
	for _, entry := range dbRegistry {
		if entry.Path == absPath {
			testDB.Close()
			writeJSON(w, 409, map[string]string{"error": "数据库已加载", "existingName": entry.Name})
			return
		}
		if entry.Name == name {
			switch body.OnConflict {
			case "rename":
				name = name + "_" + time.Now().Format("0102-1504")
			case "replace":
				// Remove old entry
				if oldConn, ok := dbConns[entry.Path]; ok {
					oldConn.Close()
					delete(dbConns, entry.Path)
				}
				dbRegistry = removeFromRegistry(dbRegistry, entry.Path)
				if activeDBPath == entry.Path {
					activeDBPath = absPath
				}
			case "skip":
				testDB.Close()
				writeJSON(w, 409, map[string]string{"error": "名称冲突: " + name, "conflict": "true"})
				return
			default:
				// "rename" default
				name = name + "_" + time.Now().Format("0102-1504")
			}
			break
		}
	}

	usedColors := make(map[string]bool)
	for _, e := range dbRegistry {
		usedColors[e.Color] = true
	}
	color := pickColor(usedColors)

	db.SetMeta(testDB, "name", name)
	dbConns[absPath] = testDB

	dbRegistry = append(dbRegistry, db.DBEntry{Name: name, Path: absPath, Color: color, Editable: true})
	saveRegistry()

	var count int
	testDB.QueryRow("SELECT COUNT(*) FROM questions").Scan(&count)

	writeJSON(w, 200, map[string]interface{}{
		"ok":    "loaded",
		"name":  name,
		"path":  absPath,
		"color": color,
		"count": count,
	})
}

func removeFromRegistry(reg []db.DBEntry, path string) []db.DBEntry {
	var result []db.DBEntry
	for _, e := range reg {
		if e.Path != path {
			result = append(result, e)
		}
	}
	return result
}

func handleDBRemove(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}
	var body struct{ Path string `json:"path"` }
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		writeJSON(w, 400, map[string]string{"error": "invalid JSON"})
		return
	}

	dbMu.Lock()
	defer dbMu.Unlock()

	if len(dbRegistry) <= 1 {
		writeJSON(w, 400, map[string]string{"error": "至少保留一个数据库"})
		return
	}

	if conn, ok := dbConns[body.Path]; ok {
		conn.Close()
		delete(dbConns, body.Path)
	}
	dbRegistry = removeFromRegistry(dbRegistry, body.Path)

	if activeDBPath == body.Path {
		activeDBPath = dbRegistry[0].Path
	}
	saveRegistry()

	writeJSON(w, 200, map[string]string{"ok": "removed"})
}

// ----- Orphaned file cleanup -----

func collectReferencedFiles() map[string]bool {
	referenced := make(map[string]bool)

	dbMu.RLock()
	defer dbMu.RUnlock()

	for _, conn := range dbConns {
		rows, err := conn.Query("SELECT answer FROM questions")
		if err != nil {
			continue
		}
		for rows.Next() {
			var answer string
			if err := rows.Scan(&answer); err != nil {
				continue
			}
			matches := fileRefRe.FindAllStringSubmatch(answer, -1)
			for _, m := range matches {
				if len(m) > 1 {
					referenced[m[1]] = true
				}
			}
		}
		rows.Close()
	}
	return referenced
}

func cleanupOrphanedFiles() int {
	referenced := collectReferencedFiles()

	entries, err := os.ReadDir(filesDir)
	if err != nil {
		return 0
	}

	removed := 0
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		if !referenced[entry.Name()] {
			path := filepath.Join(filesDir, entry.Name())
			if err := os.Remove(path); err == nil {
				removed++
				debugf("清理孤儿文件: %s", entry.Name())
			}
		}
	}
	return removed
}

// ----- DB Export / Import -----

func handleDBExport(w http.ResponseWriter, r *http.Request) {
	if !requireAuth(w, r) {
		return
	}

	if n := cleanupOrphanedFiles(); n > 0 {
		debugf("导出前清理了 %d 个孤儿文件", n)
	}

	d, _, _ := resolveDB(r)
	if d == nil {
		writeJSON(w, 500, map[string]string{"error": "no database selected"})
		return
	}

	dbMu.RLock()
	d.Exec("PRAGMA wal_checkpoint(TRUNCATE)")
	dbMu.RUnlock()

	w.Header().Set("Content-Type", "application/zip")
	w.Header().Set("Content-Disposition", `attachment; filename="qa-wiki-export.zip"`)
	zw := zip.NewWriter(w)
	defer zw.Close()

	dbParam := r.URL.Query().Get("db")
	if dbParam != "" && dbParam != "*" {
		// Export specific database from its own path
		dbMu.RLock()
		for _, entry := range dbRegistry {
			if entry.Name == dbParam {
				addFileToZip(zw, entry.Path, "data.db")
				break
			}
		}
		dbMu.RUnlock()
	} else {
		addFileToZip(zw, activeDBPath, "data.db")
	}

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

	if err := r.ParseMultipartForm(200 << 20); err != nil {
		writeJSON(w, 400, map[string]string{"error": "parse error: " + err.Error()})
		return
	}
	uploadedFile, header, err := r.FormFile("file")
	if err != nil {
		writeJSON(w, 400, map[string]string{"error": "missing file: " + err.Error()})
		return
	}
	defer uploadedFile.Close()

	mode := r.FormValue("mode")
	if mode == "" {
		mode = "overwrite"
	}
	conflict := r.FormValue("conflict")
	if conflict == "" {
		conflict = "overwrite"
	}

	filename := strings.ToLower(header.Filename)

	if mode == "load_new" {
		handleDBImportLoadNew(w, r, uploadedFile, header.Filename)
		return
	}

	if strings.HasSuffix(filename, ".zip") {
		handleDBImportZip(w, r, uploadedFile, header.Filename, mode, conflict)
	} else {
		handleDBImportRaw(w, r, uploadedFile, header.Filename, mode, conflict)
	}
}

func handleDBImportLoadNew(w http.ResponseWriter, r *http.Request, uploadedFile io.Reader, originalName string) {
	filename := strings.ToLower(originalName)

	if strings.HasSuffix(filename, ".zip") {
		tmpZip, err := os.Create(dbPath + ".load_new.zip")
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": "创建临时文件失败: " + err.Error()})
			return
		}
		defer os.Remove(tmpZip.Name())

		if _, err := io.Copy(tmpZip, uploadedFile); err != nil {
			tmpZip.Close()
			writeJSON(w, 500, map[string]string{"error": "写入临时文件失败: " + err.Error()})
			return
		}
		tmpZip.Close()

		zr, err := zip.OpenReader(tmpZip.Name())
		if err != nil {
			writeJSON(w, 400, map[string]string{"error": "无效的 zip 文件: " + err.Error()})
			return
		}
		defer zr.Close()

		var foundDB bool
		for _, f := range zr.File {
			if f.Name == "data.db" {
				foundDB = true
				break
			}
		}
		if !foundDB {
			writeJSON(w, 400, map[string]string{"error": "zip 中没有找到 data.db"})
			return
		}

		destPath := safeDBPath(strings.TrimSuffix(originalName, ".zip") + ".db")

		if err := extractZipFile(zr, "data.db", destPath); err != nil {
			writeJSON(w, 400, map[string]string{"error": "提取 data.db 失败: " + err.Error()})
			return
		}

		testDB, err := db.Open(destPath)
		if err != nil {
			os.Remove(destPath)
			writeJSON(w, 400, map[string]string{"error": "无效的数据库文件: " + err.Error()})
			return
		}
		var tableName string
		err = testDB.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='questions'").Scan(&tableName)
		testDB.Close()
		if err != nil || tableName != "questions" {
			os.Remove(destPath)
			writeJSON(w, 400, map[string]string{"error": "数据库不包含 questions 表"})
			return
		}

		for _, f := range zr.File {
			if strings.HasPrefix(f.Name, "files/") && !f.FileInfo().IsDir() {
				fileDest := filepath.Join(filesDir, strings.TrimPrefix(f.Name, "files/"))
				os.MkdirAll(filepath.Dir(fileDest), 0755)
				extractZipFile(zr, f.Name, fileDest)
			}
		}

		name := ""
		if d, er := db.Open(destPath); er == nil {
			if n, _ := db.GetMeta(d, "name"); n != "" {
				name = n
			}
			d.Close()
		}
		if name == "" {
			name = strings.TrimSuffix(filepath.Base(originalName), ".zip")
		}
		registerNewDB(destPath, name, w)
		return
	}

	destPath := safeDBPath(originalName)
	dst, err := os.Create(destPath)
	if err != nil {
		writeJSON(w, 500, map[string]string{"error": "创建数据库文件失败: " + err.Error()})
		return
	}
	if _, err := io.Copy(dst, uploadedFile); err != nil {
		dst.Close()
		os.Remove(destPath)
		writeJSON(w, 500, map[string]string{"error": "写入数据库文件失败: " + err.Error()})
		return
	}
	dst.Close()

	testDB, err := db.Open(destPath)
	if err != nil {
		os.Remove(destPath)
		writeJSON(w, 400, map[string]string{"error": "无效的数据库文件: " + err.Error()})
		return
	}
	var tableName string
	err = testDB.QueryRow("SELECT name FROM sqlite_master WHERE type='table' AND name='questions'").Scan(&tableName)
	testDB.Close()
	if err != nil || tableName != "questions" {
		os.Remove(destPath)
		writeJSON(w, 400, map[string]string{"error": "数据库不包含 questions 表"})
		return
	}

	registerNewDB(destPath, "", w)
}

func handleDBImportZip(w http.ResponseWriter, r *http.Request, uploadedFile io.Reader, originalName string, mode string, conflict string) {
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

	var dbEntry string
	for _, f := range zr.File {
		if f.Name == "data.db" {
			dbEntry = f.Name
			break
		}
	}
	if dbEntry == "" {
		writeJSON(w, 400, map[string]string{"error": "zip 中没有找到 data.db"})
		return
	}

	tmpDBPath := dbPath + ".import"
	if err := extractZipFile(zr, "data.db", tmpDBPath); err != nil {
		writeJSON(w, 400, map[string]string{"error": "提取 data.db 失败: " + err.Error()})
		return
	}
	defer os.Remove(tmpDBPath)

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

	database := activeDB()

	dbMu.Lock()
	database.Exec("PRAGMA wal_checkpoint(TRUNCATE)")
	dbMu.Unlock()

	if mode == "merge" {
		merged, err := mergeImportedDB(tmpDBPath, conflict)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": "合并失败: " + err.Error()})
			return
		}

		filesMerged := 0
		for _, f := range zr.File {
			if strings.HasPrefix(f.Name, "files/") && !f.FileInfo().IsDir() {
				destPath := filepath.Join(filesDir, strings.TrimPrefix(f.Name, "files/"))
				if _, err := os.Stat(destPath); os.IsNotExist(err) {
					os.MkdirAll(filepath.Dir(destPath), 0755)
					if err := extractZipFile(zr, f.Name, destPath); err == nil {
						filesMerged++
					}
				}
			}
		}

		writeJSON(w, 200, map[string]string{
			"ok":             "merged",
			"name":           originalName,
			"mergedEntries":  fmt.Sprintf("%d", merged),
			"filesExtracted": fmt.Sprintf("%d", filesMerged),
		})
		return
	}

	// Overwrite mode
	backupPath := activeDBPath + ".backup"
	dbMu.Lock()
	oldDB := dbConns[activeDBPath]
	dbConns[activeDBPath] = nil
	dbMu.Unlock()
	if oldDB != nil {
		oldDB.Close()
	}

	os.Remove(backupPath)
	os.Rename(activeDBPath, backupPath)
	if err := os.Rename(tmpDBPath, activeDBPath); err != nil {
		os.Rename(backupPath, activeDBPath)
		newDB, _ := db.Open(activeDBPath)
		db.InitSchema(newDB)
		dbMu.Lock()
		dbConns[activeDBPath] = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "替换数据库失败: " + err.Error()})
		return
	}

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

	newDB, err := db.Open(activeDBPath)
	if err != nil {
		os.Remove(activeDBPath)
		os.Rename(backupPath, activeDBPath)
		newDB, _ = db.Open(activeDBPath)
		db.InitSchema(newDB)
		dbMu.Lock()
		dbConns[activeDBPath] = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "打开数据库失败: " + err.Error()})
		return
	}

	dbMu.Lock()
	dbConns[activeDBPath] = newDB
	dbMu.Unlock()

	writeJSON(w, 200, map[string]string{
		"ok":             "imported",
		"name":           originalName,
		"backup":         backupPath,
		"filesExtracted": fmt.Sprintf("%d", filesExtracted),
	})
}

func handleDBImportRaw(w http.ResponseWriter, r *http.Request, uploadedFile io.Reader, originalName string, mode string, conflict string) {
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

	database := activeDB()

	dbMu.Lock()
	database.Exec("PRAGMA wal_checkpoint(TRUNCATE)")
	dbMu.Unlock()

	if mode == "merge" {
		merged, err := mergeImportedDB(tmpPath, conflict)
		if err != nil {
			writeJSON(w, 500, map[string]string{"error": "合并失败: " + err.Error()})
			return
		}
		writeJSON(w, 200, map[string]string{
			"ok":            "merged",
			"name":          originalName,
			"mergedEntries": fmt.Sprintf("%d", merged),
		})
		return
	}

	// Overwrite mode
	backupPath := activeDBPath + ".backup"
	dbMu.Lock()
	oldDB := dbConns[activeDBPath]
	dbConns[activeDBPath] = nil
	dbMu.Unlock()
	if oldDB != nil {
		oldDB.Close()
	}

	os.Remove(backupPath)
	os.Rename(activeDBPath, backupPath)
	if err := os.Rename(tmpPath, activeDBPath); err != nil {
		os.Rename(backupPath, activeDBPath)
		newDB, _ := db.Open(activeDBPath)
		db.InitSchema(newDB)
		dbMu.Lock()
		dbConns[activeDBPath] = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "替换数据库失败: " + err.Error()})
		return
	}

	newDB, err := db.Open(activeDBPath)
	if err != nil {
		os.Remove(activeDBPath)
		os.Rename(backupPath, activeDBPath)
		newDB, _ = db.Open(activeDBPath)
		db.InitSchema(newDB)
		dbMu.Lock()
		dbConns[activeDBPath] = newDB
		dbMu.Unlock()
		writeJSON(w, 500, map[string]string{"error": "打开数据库失败: " + err.Error()})
		return
	}

	dbMu.Lock()
	dbConns[activeDBPath] = newDB
	dbMu.Unlock()

	writeJSON(w, 200, map[string]string{
		"ok":     "imported",
		"name":   originalName,
		"backup": backupPath,
	})
}

func mergeImportedDB(importPath string, conflict string) (int, error) {
	importDB, err := db.Open(importPath)
	if err != nil {
		return 0, fmt.Errorf("打开导入数据库失败: %w", err)
	}
	defer importDB.Close()

	rows, err := importDB.Query("SELECT id, question, answer, category, visibility, author, starred, created_at, updated_at FROM questions ORDER BY id")
	if err != nil {
		return 0, fmt.Errorf("读取导入数据库失败: %w", err)
	}
	defer rows.Close()

	database := activeDB()

	dbMu.Lock()
	defer dbMu.Unlock()

	count := 0
	for rows.Next() {
		var q db.QA
		var starred int
		if err := rows.Scan(&q.ID, &q.Question, &q.Answer, &q.Category, &q.Visibility, &q.Author, &starred, &q.CreatedAt, &q.UpdatedAt); err != nil {
			continue
		}
		q.Starred = starred != 0

		var execErr error
		switch conflict {
		case "skip":
			_, execErr = database.Exec(
				"INSERT OR IGNORE INTO questions (id, question, answer, category, visibility, author, starred, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
				q.ID, q.Question, q.Answer, q.Category, q.Visibility, q.Author, starred, q.CreatedAt, q.UpdatedAt,
			)
		case "append":
			_, execErr = database.Exec(
				"INSERT INTO questions (question, answer, category, visibility, author, starred, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
				q.Question, q.Answer, q.Category, q.Visibility, q.Author, starred, q.CreatedAt, q.UpdatedAt,
			)
		default:
			_, execErr = database.Exec(
				"INSERT OR REPLACE INTO questions (id, question, answer, category, visibility, author, starred, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
				q.ID, q.Question, q.Answer, q.Category, q.Visibility, q.Author, starred, q.CreatedAt, q.UpdatedAt,
			)
		}
		if execErr == nil {
			count++
		}
	}
	return count, rows.Err()
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
