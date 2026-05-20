package main

import (
	"database/sql"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"

	_ "modernc.org/sqlite"
)

func main() {
	dbPath := "data.db"
	if len(os.Args) > 1 {
		dbPath = os.Args[1]
	}

	if _, err := os.Stat(dbPath); os.IsNotExist(err) {
		log.Fatalf("源数据库不存在: %s", dbPath)
	}

	srcDB, err := sql.Open("sqlite", dbPath)
	if err != nil {
		log.Fatalf("打开源数据库失败: %v", err)
	}
	defer srcDB.Close()

	dir := filepath.Dir(dbPath)
	dstPath := filepath.Join(dir, "public_data.db")
	os.Remove(dstPath)

	dstDB, err := sql.Open("sqlite", dstPath)
	if err != nil {
		log.Fatalf("创建目标数据库失败: %v", err)
	}
	defer dstDB.Close()

	if _, err := dstDB.Exec(`
		CREATE TABLE IF NOT EXISTS questions (
			id INTEGER PRIMARY KEY,
			question TEXT NOT NULL,
			answer TEXT NOT NULL DEFAULT '',
			category TEXT NOT NULL DEFAULT '',
			visibility TEXT NOT NULL DEFAULT 'public' CHECK(visibility IN ('internal', 'public')),
			updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
		)
	`); err != nil {
		log.Fatalf("创建目标表失败: %v", err)
	}

	rows, err := srcDB.Query("SELECT id, question, answer, category, visibility, updated_at FROM questions WHERE visibility = 'public'")
	if err != nil {
		log.Fatalf("查询源数据库失败: %v", err)
	}
	defer rows.Close()

	tx, err := dstDB.Begin()
	if err != nil {
		log.Fatalf("开始事务失败: %v", err)
	}

	stmt, err := tx.Prepare("INSERT INTO questions (id, question, answer, category, visibility, updated_at) VALUES (?, ?, ?, ?, ?, ?)")
	if err != nil {
		log.Fatalf("预编译语句失败: %v", err)
	}
	defer stmt.Close()

	var exported int
	var skipped []int

	for rows.Next() {
		var id int
		var question, answer, category, visibility, updatedAt string
		if err := rows.Scan(&id, &question, &answer, &category, &visibility, &updatedAt); err != nil {
			log.Fatalf("读取行失败: %v", err)
		}
		if _, err := stmt.Exec(id, question, answer, category, "public", updatedAt); err != nil {
			log.Fatalf("写入行失败: %v", err)
		}
		exported++
	}

	// Count skipped (internal) records
	var total int
	srcDB.QueryRow("SELECT COUNT(*) FROM questions").Scan(&total)
	var internalCount int
	srcDB.QueryRow("SELECT COUNT(*) FROM questions WHERE visibility = 'internal'").Scan(&internalCount)

	// Find skipped IDs
	internalRows, _ := srcDB.Query("SELECT id FROM questions WHERE visibility = 'internal'")
	if internalRows != nil {
		defer internalRows.Close()
		for internalRows.Next() {
			var id int
			internalRows.Scan(&id)
			skipped = append(skipped, id)
		}
	}

	if err := tx.Commit(); err != nil {
		log.Fatalf("提交事务失败: %v", err)
	}

	if _, err := dstDB.Exec("VACUUM"); err != nil {
		log.Fatalf("VACUUM 失败: %v", err)
	}

	fmt.Printf("导出完成\n")
	fmt.Printf("  源数据库:   %s (%d 条记录)\n", dbPath, total)
	fmt.Printf("  导出记录:   %d 条 (public)\n", exported)
	if len(skipped) > 0 {
		ids := strings.Trim(strings.Replace(fmt.Sprint(skipped), " ", ", ", -1), "[]")
		fmt.Printf("  已过滤:     %d 条 (internal, ID: %s)\n", internalCount, ids)
	}
	fmt.Printf("  目标文件:   %s\n", dstPath)
}
