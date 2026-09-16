#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <sqlite3.h>

#define PORT 8080
#define PAGE_SIZE 50
#define DB_NAME "smart_hr.db"

sqlite3 *db; // 全局数据库连接

// ================= 数据库操作 =================

// 初始化数据库：建表 + 首次导入 names.txt
void init_db() {
    int rc = sqlite3_open(DB_NAME, &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        exit(1);
    }
    printf("✅ 数据库连接成功。\n");

    // 1. 创建表
    char *sql_create = "CREATE TABLE IF NOT EXISTS employees ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "name TEXT NOT NULL,"
                       "emp_id TEXT UNIQUE NOT NULL,"
                       "is_deleted INTEGER DEFAULT 0);";
    char *err_msg = 0;
    rc = sqlite3_exec(db, sql_create, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    // 2. 检查是否为空，如果是空则导入 names.txt
    int count = 0;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM employees;", -1, &stmt, 0);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        printf("📂 数据库为空，正在从 names.txt 导入数据 (可能需要几秒)...\n");
        FILE *file = fopen("names.txt", "r");
        if (file) {
            char line[64];
            int id = 1;
            // 开启事务，极大提高插入速度
            sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, 0);
            while (fgets(line, sizeof(line), file)) {
                line[strcspn(line, "\n")] = 0;
                if (strlen(line) > 0) {
                    char emp_id[20];
                    sprintf(emp_id, "2026%04d", id);
                    char sql[256];
                    sprintf(sql, "INSERT INTO employees (name, emp_id) VALUES ('%s', '%s');", line, emp_id);
                    sqlite3_exec(db, sql, 0, 0, 0);
                    id++;
                }
            }
            sqlite3_exec(db, "COMMIT;", 0, 0, 0);
            fclose(file);
            printf("✅ 成功导入 %d 名员工到数据库。\n", id - 1);
        } else {
            printf("️ 警告: 找不到 names.txt，数据库将为空。\n");
        }
    } else {
        printf("✅ 数据库已有 %d 条记录，跳过导入。\n", count);
    }
}

void to_lowercase(char *str) {
    for (int i = 0; str[i]; i++) str[i] = tolower(str[i]);
}

// ================= HTTP 服务基础 =================

void serve_file(int client_fd, const char *filename, const char *content_type) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
        write(client_fd, nf, strlen(nf));
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(size + 1);
    fread(buf, 1, size, f);
    buf[size] = 0;
    fclose(f);

    char header[512];
    int hlen = sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n", content_type, size);
    write(client_fd, header, hlen);
    write(client_fd, buf, size);
    free(buf);
}

// ================= API 处理 =================

// 1. 登录 API
void handle_login(int client_fd, char *body) {
    char name[64] = "", emp_id[20] = "";
    char *name_pos = strstr(body, "name=");
    if (name_pos) sscanf(name_pos, "name=%63[^&]", name);
    char *id_pos = strstr(body, "emp_id=");
    if (id_pos) sscanf(id_pos, "emp_id=%19[^&\n ]", emp_id);

    char sql[256];
    sprintf(sql, "SELECT emp_id FROM employees WHERE name='%s' AND emp_id='%s' AND is_deleted=0;", name, emp_id);
    
    sqlite3_stmt *stmt;
    int found = 0, is_boss = 0;
    sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = 1;
        if (strcmp(emp_id, "20260001") == 0) is_boss = 1;
    }
    sqlite3_finalize(stmt);

    char response[512];
    if (found) {
        sprintf(response, "{\"success\":true,\"name\":\"%s\",\"emp_id\":\"%s\",\"is_boss\":%s}", name, emp_id, is_boss ? "true" : "false");
    } else {
        sprintf(response, "{\"success\":false,\"message\":\"名字或工号错误\"}");
    }

    char header[256];
    int hlen = sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(response));
    write(client_fd, header, hlen);
    write(client_fd, response, strlen(response));
}

// 2. 获取员工列表 API (分页 + 搜索)
void handle_get_staff(int client_fd, char *path) {
    int page = 1;
    char search[64] = "";

    char *page_pos = strstr(path, "page=");
    if (page_pos) sscanf(page_pos, "page=%d", &page);
    char *search_pos = strstr(path, "search=");
    if (search_pos) sscanf(search_pos, "search=%63[^&\n ]", search);
    to_lowercase(search);
    if (page < 1) page = 1;

    // 1. 查总数
    char count_sql[512];
    if (strlen(search) == 0) {
        sprintf(count_sql, "SELECT COUNT(*) FROM employees WHERE is_deleted=0;");
    } else {
        // 使用 LIKE 进行模糊搜索，且转小写匹配 (SQLite 默认大小写敏感，这里用 LOWER)
        sprintf(count_sql, "SELECT COUNT(*) FROM employees WHERE is_deleted=0 AND LOWER(name) LIKE '%%%s%%';", search);
    }
    
    int total = 0;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, count_sql, -1, &stmt, 0);
    if (sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    int total_pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
    if (total_pages < 1) total_pages = 1;
    if (page > total_pages) page = total_pages;
    int offset = (page - 1) * PAGE_SIZE;

    // 2. 查列表
    char list_sql[512];
    if (strlen(search) == 0) {
        sprintf(list_sql, "SELECT id, name, emp_id FROM employees WHERE is_deleted=0 LIMIT %d OFFSET %d;", PAGE_SIZE, offset);
    } else {
        sprintf(list_sql, "SELECT id, name, emp_id FROM employees WHERE is_deleted=0 AND LOWER(name) LIKE '%%%s%%' LIMIT %d OFFSET %d;", search, PAGE_SIZE, offset);
    }

    char response[65536];
    int res_offset = sprintf(response, "{\"total\":%d,\"pages\":%d,\"page\":%d,\"list\":[", total, total_pages, page);

    sqlite3_prepare_v2(db, list_sql, -1, &stmt, 0);
    int first = 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *emp_id = (const char *)sqlite3_column_text(stmt, 2);
        
        if (!first) { response[res_offset++] = ','; response[res_offset] = 0; }
        res_offset += sprintf(response + res_offset, "{\"id\":%d,\"name\":\"%s\",\"emp_id\":\"%s\"}", id, name, emp_id);
        first = 0;
    }
    sqlite3_finalize(stmt);
    strcat(response + res_offset, "]}");

    char header[512];
    int hlen = sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(response));
    write(client_fd, header, hlen);
    write(client_fd, response, strlen(response));
}

// 3. 删除员工 API
void handle_delete(int client_fd, char *body) {
    int id = 0;
    char *id_pos = strstr(body, "id=");
    if (id_pos) sscanf(id_pos, "id=%d", &id);

    char sql[128];
    sprintf(sql, "UPDATE employees SET is_deleted=1 WHERE id=%d;", id);
    sqlite3_exec(db, sql, 0, 0, 0);
    printf("✅ 已软删除员工 ID=%d\n", id);

    char *resp = "{\"success\":true}";
    char header[256];
    int hlen = sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(resp));
    write(client_fd, header, hlen);
    write(client_fd, resp, strlen(resp));
}

// 4. 新增员工 API
void handle_add(int client_fd, char *body) {
    char name[64] = "";
    char *name_pos = strstr(body, "name=");
    if (name_pos) sscanf(name_pos, "name=%63[^&]", name);

    if (strlen(name) == 0) {
        char *err = "{\"success\":false,\"message\":\"名字不能为空\"}";
        write(client_fd, err, strlen(err));
        return;
    }

    // 获取当前最大 ID 生成新工号 (简单处理)
    int new_id = 0;
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT MAX(id) FROM employees;", -1, &stmt, 0);
    if (sqlite3_step(stmt) == SQLITE_ROW) new_id = sqlite3_column_int(stmt, 0) + 1;
    sqlite3_finalize(stmt);
    if (new_id == 0) new_id = 1;

    char emp_id[20];
    sprintf(emp_id, "2026%04d", new_id); // 注意：这里为了简单复用 ID 逻辑，实际业务工号可能独立

    char sql[256];
    sprintf(sql, "INSERT INTO employees (name, emp_id) VALUES ('%s', '%s');", name, emp_id);
    sqlite3_exec(db, sql, 0, 0, 0);
    printf("✅ 已新增员工: %s\n", name);

    char resp[256];
    sprintf(resp, "{\"success\":true,\"id\":%d}", new_id);
    char header[256];
    int hlen = sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(resp));
    write(client_fd, header, hlen);
    write(client_fd, resp, strlen(resp));
}

// 5. 修改员工名字 API
void handle_update(int client_fd, char *body) {
    int id = 0;
    char name[64] = "";
    char *id_pos = strstr(body, "id=");
    if (id_pos) sscanf(id_pos, "id=%d", &id);
    char *name_pos = strstr(body, "name=");
    if (name_pos) sscanf(name_pos, "name=%63[^&]", name);

    char sql[256];
    sprintf(sql, "UPDATE employees SET name='%s' WHERE id=%d;", name, id);
    sqlite3_exec(db, sql, 0, 0, 0);
    printf("✅ 已修改员工 ID=%d 名字为 %s\n", id, name);

    char *resp = "{\"success\":true}";
    char header[256];
    int hlen = sprintf(header, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(resp));
    write(client_fd, header, hlen);
    write(client_fd, resp, strlen(resp));
}

int main() {
    init_db(); // 初始化数据库

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 10);

    printf("🚀 服务器已启动: http://localhost:%d\n", PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        char buffer[8192] = {0};
        read(client_fd, buffer, sizeof(buffer) - 1);

        char method[16], path[256];
        sscanf(buffer, "%s %s", method, path);

        if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
            serve_file(client_fd, "index.html", "text/html; charset=utf-8");
        } else if (strcmp(method, "GET") == 0 && strncmp(path, "/api/staff", 10) == 0) {
            handle_get_staff(client_fd, path);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/login") == 0) {
            char *body = strstr(buffer, "\r\n\r\n");
            if (body) handle_login(client_fd, body + 4);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/staff/delete") == 0) {
            char *body = strstr(buffer, "\r\n\r\n");
            if (body) handle_delete(client_fd, body + 4);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/staff/add") == 0) {
            char *body = strstr(buffer, "\r\n\r\n");
            if (body) handle_add(client_fd, body + 4);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/staff/update") == 0) {
            char *body = strstr(buffer, "\r\n\r\n");
            if (body) handle_update(client_fd, body + 4);
        } else {
            char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nNot Found";
            write(client_fd, nf, strlen(nf));
        }
        close(client_fd);
    }
    sqlite3_close(db);
    return 0;
}