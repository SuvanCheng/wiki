package db

import (
	"database/sql"
	"strings"
	"time"

	_ "modernc.org/sqlite"
)

type QA struct {
	ID         int    `json:"id"`
	Question   string `json:"question"`
	Answer     string `json:"answer"`
	Category   string `json:"category"`
	Visibility string `json:"visibility"`
	CreatedAt  string `json:"created_at"`
	UpdatedAt  string `json:"updated_at"`
}

type CatCount struct {
	Name  string `json:"name"`
	Count int    `json:"count"`
}

type Stats struct {
	Total         int        `json:"total"`
	PublicCount   int        `json:"public_count"`
	InternalCount int        `json:"internal_count"`
	Categories    []CatCount `json:"categories"`
	Recent        []QA       `json:"recent"`
}

func Open(dbPath string) (*sql.DB, error) {
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	db.Exec("PRAGMA journal_mode=WAL")
	db.Exec("PRAGMA busy_timeout=5000")
	return db, nil
}

func InitSchema(db *sql.DB) error {
	_, err := db.Exec(`
		CREATE TABLE IF NOT EXISTS questions (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			question TEXT NOT NULL,
			answer TEXT NOT NULL DEFAULT '',
			category TEXT NOT NULL DEFAULT '',
			visibility TEXT NOT NULL DEFAULT 'internal' CHECK(visibility IN ('internal', 'public')),
			created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
			updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)
	`)
	if err != nil {
		return err
	}
	// Migrate: add created_at to older databases, backfill from updated_at
	db.Exec("ALTER TABLE questions ADD COLUMN created_at DATETIME")
	db.Exec("UPDATE questions SET created_at = updated_at WHERE created_at IS NULL")
	return nil
}

func Count(db *sql.DB) (int, error) {
	var count int
	err := db.QueryRow("SELECT COUNT(*) FROM questions").Scan(&count)
	return count, err
}

func InsertMockData(db *sql.DB) error {
	now := time.Now().Format(time.RFC3339)
	mocks := []QA{
		{
			Question:   "公司内部服务器 IP 地址和访问凭证是什么？",
			Answer:     "## 内部服务器信息\n\n| 环境 | IP 地址 | 用途 |\n|------|---------|------|\n| 测试环境 | `192.168.1.100` | 日常开发测试 |\n| 预发布 | `192.168.1.200` | 上线前验证 |\n| 生产环境 | `10.0.0.50` | 正式服务 |\n\n**注意：** SSH 密钥存放在内部 Vault，请联系运维获取。",
			Category:   "内部信息",
			Visibility: "internal",
		},
		{
			Question:   "如何在 Go 中实现并发？",
			Answer:     "## Goroutine 基础\n\nGo 使用 `goroutine` 实现轻量级并发：\n\n```go\npackage main\n\nimport (\n\t\"fmt\"\n\t\"sync\"\n)\n\nfunc main() {\n\tvar wg sync.WaitGroup\n\tfor i := 0; i < 5; i++ {\n\t\twg.Add(1)\n\t\tgo func(id int) {\n\t\t\tdefer wg.Done()\n\t\t\tfmt.Printf(\"goroutine %d\\n\", id)\n\t\t}(i)\n\t}\n\twg.Wait()\n}\n```\n\n## Channel 通信\n\n```go\nch := make(chan string, 10)\nch <- \"hello\"\nmsg := <-ch\n```\n\n## 常用并发模式\n\n| 模式 | 用途 | 场景 |\n|------|------|------|\n| Fan-Out | 一个生产者多个消费者 | 并行处理任务 |\n| Fan-In | 多个生产者一个消费者 | 聚合结果 |\n| Pipeline | 数据流经多个阶段 | 数据处理管道 |\n| Worker Pool | 固定数量协程处理 | 限制并发数 |\n\n> **提示：** 使用 `sync.WaitGroup` 等待 goroutine 完成，使用 `context.Context` 控制超时和取消。",
			Category:   "编程",
			Visibility: "public",
		},
		{
			Question:   "项目如何部署？",
			Answer:     "## 部署步骤\n\n1. **构建二进制**\n   ```bash\n   CGO_ENABLED=0 go build -o qa-wiki ./cmd/qa-wiki\n   ```\n\n2. **准备数据文件**\n   - 将 `data.db` 放在与可执行文件同级目录\n   - 确保数据库文件有读写权限\n\n3. **启动服务**\n   ```bash\n   ./qa-wiki\n   ```\n\n4. **验证**\n   - 浏览器会自动打开 `http://127.0.0.1:8080`\n   - 确认页面正常加载，搜索功能可用\n\n## 运行环境要求\n\n| 平台 | 最低版本 |\n|------|----------|\n| macOS | 11+ (Intel / Apple Silicon) |\n| Windows | 10+ (x86_64) |\n| Linux | Kernel 3.10+ (x86_64) |\n\n> 无需安装任何运行时依赖，二进制文件即为完整应用。",
			Category:   "运维",
			Visibility: "public",
		},
	}

	for _, m := range mocks {
		_, err := db.Exec(
			"INSERT INTO questions (question, answer, category, visibility, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)",
			m.Question, m.Answer, m.Category, m.Visibility, now, now,
		)
		if err != nil {
			return err
		}
	}
	return nil
}

func GetByID(db *sql.DB, id int) (QA, error) {
	var q QA
	err := db.QueryRow(
		"SELECT id, question, answer, category, visibility, created_at, updated_at FROM questions WHERE id = ?", id,
	).Scan(&q.ID, &q.Question, &q.Answer, &q.Category, &q.Visibility, &q.CreatedAt, &q.UpdatedAt)
	return q, err
}

func Insert(db *sql.DB, q *QA) error {
	now := time.Now().Format(time.RFC3339)
	q.CreatedAt = now
	q.UpdatedAt = now
	result, err := db.Exec(
		"INSERT INTO questions (question, answer, category, visibility, created_at, updated_at) VALUES (?, ?, ?, ?, ?, ?)",
		q.Question, q.Answer, q.Category, q.Visibility, q.CreatedAt, q.UpdatedAt,
	)
	if err != nil {
		return err
	}
	id, err := result.LastInsertId()
	if err != nil {
		return err
	}
	q.ID = int(id)
	return nil
}

func Update(db *sql.DB, q *QA) error {
	q.UpdatedAt = time.Now().Format(time.RFC3339)
	_, err := db.Exec(
		"UPDATE questions SET question=?, answer=?, category=?, visibility=?, updated_at=? WHERE id=?",
		q.Question, q.Answer, q.Category, q.Visibility, q.UpdatedAt, q.ID,
	)
	return err
}

func Delete(db *sql.DB, id int) error {
	_, err := db.Exec("DELETE FROM questions WHERE id = ?", id)
	return err
}

func Search(db *sql.DB, keyword string, includeInternal bool) ([]QA, error) {
	var rows *sql.Rows
	var err error

	baseQuery := "SELECT id, question, answer, category, visibility, created_at, updated_at FROM questions"
	orderClause := " ORDER BY updated_at DESC"

	if keyword == "" {
		if includeInternal {
			rows, err = db.Query(baseQuery + orderClause)
		} else {
			rows, err = db.Query(baseQuery+" WHERE visibility = 'public'"+orderClause)
		}
	} else {
		like := "%" + keyword + "%"
		whereClause := " WHERE (question LIKE ? OR answer LIKE ? OR category LIKE ?)"
		if !includeInternal {
			whereClause += " AND visibility = 'public'"
		}
		rows, err = db.Query(baseQuery+whereClause+orderClause, like, like, like)
	}
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var results []QA
	for rows.Next() {
		var q QA
		if err := rows.Scan(&q.ID, &q.Question, &q.Answer, &q.Category, &q.Visibility, &q.CreatedAt, &q.UpdatedAt); err != nil {
			return nil, err
		}
		results = append(results, q)
	}
	if results == nil {
		results = []QA{}
	}
	return results, rows.Err()
}

func GetStats(db *sql.DB, includeInternal bool) (Stats, error) {
	var s Stats

	// totals
	if includeInternal {
		db.QueryRow("SELECT COUNT(*) FROM questions").Scan(&s.Total)
	} else {
		db.QueryRow("SELECT COUNT(*) FROM questions WHERE visibility = 'public'").Scan(&s.Total)
	}
	db.QueryRow("SELECT COUNT(*) FROM questions WHERE visibility = 'public'").Scan(&s.PublicCount)

	var internalCount int
	db.QueryRow("SELECT COUNT(*) FROM questions WHERE visibility = 'internal'").Scan(&internalCount)
	if includeInternal {
		s.InternalCount = internalCount
	}

	// categories: split comma-separated tags and count individually
	catQuery := "SELECT category FROM questions WHERE category != ''"
	if !includeInternal {
		catQuery += " AND visibility = 'public'"
	}
	catRows, err := db.Query(catQuery)
	if err == nil {
		defer catRows.Close()
		catMap := make(map[string]int)
		for catRows.Next() {
			var cat string
			if err := catRows.Scan(&cat); err == nil {
				for _, tag := range splitAndTrim(cat) {
					catMap[tag]++
				}
			}
		}
		// Convert map to sorted slice (by count desc)
		type kv struct {
			k string
			v int
		}
		var pairs []kv
		for k, v := range catMap {
			pairs = append(pairs, kv{k, v})
		}
		// Sort by count desc, then name asc
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
			s.Categories = append(s.Categories, CatCount{Name: pairs[i].k, Count: pairs[i].v})
		}
	}
	if s.Categories == nil {
		s.Categories = []CatCount{}
	}

	// recent 5
	recentQuery := "SELECT id, question, answer, category, visibility, created_at, updated_at FROM questions"
	if !includeInternal {
		recentQuery += " WHERE visibility = 'public'"
	}
	recentQuery += " ORDER BY updated_at DESC LIMIT 5"
	recRows, err := db.Query(recentQuery)
	if err == nil {
		defer recRows.Close()
		for recRows.Next() {
			var q QA
			if err := recRows.Scan(&q.ID, &q.Question, &q.Answer, &q.Category, &q.Visibility, &q.CreatedAt, &q.UpdatedAt); err == nil {
				s.Recent = append(s.Recent, q)
			}
		}
	}
	if s.Recent == nil {
		s.Recent = []QA{}
	}

	return s, nil
}

func splitAndTrim(s string) []string {
	parts := strings.Split(s, ",")
	var result []string
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			result = append(result, p)
		}
	}
	return result
}
