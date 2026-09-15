#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#define PORT 8080
#define MAX_EMPLOYEES 35000
#define PAGE_SIZE 50

typedef struct {
    int id;
    char name[64];
    char emp_id[20];
    int is_deleted;
} Employee;

Employee db[MAX_EMPLOYEES];
int db_count = 0;
static int match_idx[MAX_EMPLOYEES];

// 1. 从 names.txt 加载原始数据
void load_data() {
    FILE *file = fopen("names.txt", "r");
    if (!file) { printf("错误: 找不到 names.txt！\n"); exit(1); }
    char line[64];
    int id = 1;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0) {
            db[db_count].id = id;
            strcpy(db[db_count].name, line);
            sprintf(db[db_count].emp_id, "2026%04d", id);
            db[db_count].is_deleted = 0;
            db_count++;
            id++;
        }
    }
    fclose(file);
    printf("✅ 从 names.txt 加载 %d 名员工数据。\n", db_count);
}

// 2. 优先从 staff.db 加载（持久化数据）
void load_from_db() {
    FILE *file = fopen("staff.db", "r");
    if (!file) return;
    printf("📂 从 staff.db 加载数据...\n");
    db_count = 0;
    char line[64];
    int id = 1;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        if (strlen(line) > 0) {
            db[db_count].id = id;
            strcpy(db[db_count].name, line);
            sprintf(db[db_count].emp_id, "2026%04d", id);
            db[db_count].is_deleted = 0;
            db_count++;
            id++;
        }
    }
    fclose(file);
    printf("✅ 从 staff.db 加载 %d 名员工。\n", db_count);
}

// 3. 保存数据到 staff.db
void save_to_db() {
    FILE *f = fopen("staff.db", "w");
    if (!f) return;
    for (int i = 0; i < db_count; i++) {
        if (!db[i].is_deleted) {
            fprintf(f, "%s\n", db[i].name);
        }
    }
    fclose(f);
}

void to_lowercase(char *str) {
    for (int i = 0; str[i]; i++) str[i] = tolower(str[i]);
}

// 4. 发送 HTML 文件
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
    int hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
        content_type, size);
    write(client_fd, header, hlen);
    write(client_fd, buf, size);
    free(buf);
}

// 5. 登录 API
void handle_login(int client_fd, char *body) {
    char name[64] = "", emp_id[20] = "";
    char *name_pos = strstr(body, "name=");
    if (name_pos) sscanf(name_pos, "name=%63[^&]", name);
    char *id_pos = strstr(body, "emp_id=");
    if (id_pos) sscanf(id_pos, "emp_id=%19[^&\n ]", emp_id);

    int found = 0, is_boss = 0;
    for (int i = 0; i < db_count; i++) {
        if (strcmp(db[i].name, name) == 0 && strcmp(db[i].emp_id, emp_id) == 0) {
            found = 1;
            if (strcmp(emp_id, "20260001") == 0) is_boss = 1;
            break;
        }
    }

    char response[512];
    if (found) {
        sprintf(response, "{\"success\":true,\"name\":\"%s\",\"emp_id\":\"%s\",\"is_boss\":%s}",
                name, emp_id, is_boss ? "true" : "false");
    } else {
        sprintf(response, "{\"success\":false,\"message\":\"名字或工号错误\"}");
    }

    char header[256];
    int hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(response));
    write(client_fd, header, hlen);
    write(client_fd, response, strlen(response));
}

// 6. 获取员工列表 API（支持分页和搜索）
void handle_get_staff(int client_fd, char *path) {
    int page = 1;
    char search[64] = "";

    char *page_pos = strstr(path, "page=");
    if (page_pos) sscanf(page_pos, "page=%d", &page);
    char *search_pos = strstr(path, "search=");
    if (search_pos) sscanf(search_pos, "search=%63[^&\n ]", search);
    to_lowercase(search);
    if (page < 1) page = 1;

    int match_count = 0;
    for (int i = 0; i < db_count; i++) {
        if (db[i].is_deleted) continue;
        char lower[64];
        strcpy(lower, db[i].name);
        to_lowercase(lower);
        if (strlen(search) == 0 || strstr(lower, search) != NULL) {
            match_idx[match_count++] = i;
        }
    }

    int total_pages = (match_count + PAGE_SIZE - 1) / PAGE_SIZE;
    if (total_pages < 1) total_pages = 1;
    if (page > total_pages) page = total_pages;

    int start = (page - 1) * PAGE_SIZE;
    int end = start + PAGE_SIZE;
    if (end > match_count) end = match_count;

    char response[65536];
    int offset = sprintf(response,
        "{\"total\":%d,\"pages\":%d,\"page\":%d,\"list\":[",
        match_count, total_pages, page);

    for (int k = start; k < end; k++) {
        Employee *e = &db[match_idx[k]];
        if (k > start) { response[offset++] = ','; response[offset] = 0; }
        offset += sprintf(response + offset,
            "{\"id\":%d,\"name\":\"%s\",\"emp_id\":\"%s\"}",
            e->id, e->name, e->emp_id);
    }
    strcat(response + offset, "]}");

    char header[512];
    int hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(response));
    write(client_fd, header, hlen);
    write(client_fd, response, strlen(response));
}

// 7. 删除员工 API
void handle_delete(int client_fd, char *body) {
    int id = 0;
    char *id_pos = strstr(body, "id=");
    if (id_pos) sscanf(id_pos, "id=%d", &id);

    for (int i = 0; i < db_count; i++) {
        if (db[i].id == id) {
            db[i].is_deleted = 1;
            break;
        }
    }
    save_to_db();
    printf("✅ 已删除员工 ID=%d\n", id);

    char *resp = "{\"success\":true}";
    char header[256];
    int hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(resp));
    write(client_fd, header, hlen);
    write(client_fd, resp, strlen(resp));
}

// 8. 新增员工 API
void handle_add(int client_fd, char *body) {
    char name[64] = "";
    char *name_pos = strstr(body, "name=");
    if (name_pos) sscanf(name_pos, "name=%63[^&]", name);

    if (strlen(name) == 0) {
        char *err = "{\"success\":false,\"message\":\"名字不能为空\"}";
        write(client_fd, err, strlen(err));
        return;
    }

    int new_id = db_count + 1;
    strcpy(db[db_count].name, name);
    sprintf(db[db_count].emp_id, "2026%04d", new_id);
    db[db_count].is_deleted = 0;
    db_count++;
    save_to_db();
    printf("✅ 已新增员工: %s (ID=%d)\n", name, new_id);

    char resp[256];
    sprintf(resp, "{\"success\":true,\"id\":%d}", new_id);
    char header[256];
    int hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(resp));
    write(client_fd, header, hlen);
    write(client_fd, resp, strlen(resp));
}

// 9. 修改员工名字 API
void handle_update(int client_fd, char *body) {
    int id = 0;
    char name[64] = "";
    char *id_pos = strstr(body, "id=");
    if (id_pos) sscanf(id_pos, "id=%d", &id);
    char *name_pos = strstr(body, "name=");
    if (name_pos) sscanf(name_pos, "name=%63[^&]", name);

    for (int i = 0; i < db_count; i++) {
        if (db[i].id == id) {
            strcpy(db[i].name, name);
            break;
        }
    }
    save_to_db();
    printf("✅ 已修改员工 ID=%d 名字为 %s\n", id, name);

    char *resp = "{\"success\":true}";
    char header[256];
    int hlen = sprintf(header,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", strlen(resp));
    write(client_fd, header, hlen);
    write(client_fd, resp, strlen(resp));
}

int main() {
    load_from_db();
    if (db_count == 0) load_data();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 10);

    printf("服务器已启动: http://localhost:%d\n", PORT);

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
    return 0;
}