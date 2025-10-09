// main.c (cJSON-based rewrite)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define PORT 8080
#define BUFFER_SIZE 8192

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) return 0;
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

void list_files_recursively(const char *base_path, cJSON *array) {
    DIR *dir = opendir(base_path);
    if (!dir) return;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);

        cJSON_AddItemToArray(array, cJSON_CreateString(path));

        if (entry->d_type == DT_DIR) {
            list_files_recursively(path, array);
        }
    }
    closedir(dir);
}

void send_json(int client_socket, cJSON *obj) {
    char *json_str = cJSON_PrintUnformatted(obj);
    write(client_socket, json_str, strlen(json_str));
    write(client_socket, "\n", 1);
    free(json_str);
}

void send_jsonrpc_response(int client_socket, cJSON *result, int id) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "result", result);
    cJSON_AddNumberToObject(resp, "id", id);
    send_json(client_socket, resp);
    cJSON_Delete(resp);
}

void send_jsonrpc_error(int client_socket, int code, const char *message, int id) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(resp, "error", err);
    cJSON_AddNumberToObject(resp, "id", id);
    send_json(client_socket, resp);
    cJSON_Delete(resp);
}

void handle_request(int client_socket, const char *buffer) {
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        send_jsonrpc_error(client_socket, -32700, "Parse error", 0);
        return;
    }

    cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    int id = id_item ? id_item->valueint : 0;

    if (!cJSON_IsString(method)) {
        send_jsonrpc_error(client_socket, -32600, "Invalid Request", id);
        cJSON_Delete(root);
        return;
    }

    const char *method_name = method->valuestring;

    // initialize
    if (!strcmp(method_name, "initialize")) {
        cJSON *capabilities = cJSON_CreateObject();
        cJSON *tools = cJSON_CreateObject();
        cJSON_AddBoolToObject(tools, "listChanged", 1);
        cJSON_AddItemToObject(capabilities, "tools", tools);
        cJSON *result = cJSON_CreateObject();
        cJSON_AddItemToObject(result, "capabilities", capabilities);
        send_jsonrpc_response(client_socket, result, id);
        return;
    }

    // tools/list
    if (!strcmp(method_name, "tools/list")) {
        cJSON *tools = cJSON_CreateArray();

        cJSON *t1 = cJSON_CreateObject();
        cJSON_AddStringToObject(t1, "name", "read_file");
        cJSON_AddStringToObject(t1, "description", "Read the content of a file");
        cJSON_AddItemToArray(tools, t1);

        cJSON *t2 = cJSON_CreateObject();
        cJSON_AddStringToObject(t2, "name", "recursive_file_list");
        cJSON_AddStringToObject(t2, "description", "List files recursively in a directory");
        cJSON_AddItemToArray(tools, t2);

        cJSON *t3 = cJSON_CreateObject();
        cJSON_AddStringToObject(t3, "name", "call_llm");
        cJSON_AddStringToObject(t3, "description", "Call x.ai Grok API for chat completion");
        cJSON_AddItemToArray(tools, t3);

        send_jsonrpc_response(client_socket, tools, id);
        cJSON_Delete(root);
        return;
    }

    // tools/call
    if (!strcmp(method_name, "tools/call")) {
        cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        cJSON *name_item = cJSON_GetObjectItemCaseSensitive(params, "name");
        cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");

        if (!cJSON_IsString(name_item) || !cJSON_IsObject(args)) {
            send_jsonrpc_error(client_socket, -32602, "Invalid params", id);
            cJSON_Delete(root);
            return;
        }

        const char *name = name_item->valuestring;

        // --- read_file ---
        if (!strcmp(name, "read_file")) {
            cJSON *path_item = cJSON_GetObjectItemCaseSensitive(args, "path");
            if (!cJSON_IsString(path_item)) {
                send_jsonrpc_error(client_socket, -32602, "Missing path", id);
                cJSON_Delete(root);
                return;
            }
            char safe_path[512];
            snprintf(safe_path, sizeof(safe_path), "./data/%s", path_item->valuestring);

            FILE *file = fopen(safe_path, "r");
            if (!file) {
                send_jsonrpc_error(client_socket, -32000, "File not found", id);
                cJSON_Delete(root);
                return;
            }

            char *content = malloc(BUFFER_SIZE);
            size_t len = fread(content, 1, BUFFER_SIZE - 1, file);
            fclose(file);
            content[len] = '\0';

            cJSON *result = cJSON_CreateObject();
            cJSON *content_arr = cJSON_CreateArray();
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "type", "text");
            cJSON_AddStringToObject(entry, "text", content);
            cJSON_AddItemToArray(content_arr, entry);
            cJSON_AddItemToObject(result, "content", content_arr);
            send_jsonrpc_response(client_socket, result, id);
            free(content);
            cJSON_Delete(root);
            return;
        }

        // --- recursive_file_list ---
        if (!strcmp(name, "recursive_file_list")) {
            cJSON *path_item = cJSON_GetObjectItemCaseSensitive(args, "path");
            if (!cJSON_IsString(path_item)) {
                send_jsonrpc_error(client_socket, -32602, "Missing path", id);
                cJSON_Delete(root);
                return;
            }
            char safe_path[512];
            snprintf(safe_path, sizeof(safe_path), "./data/%s", path_item->valuestring);

            cJSON *file_list = cJSON_CreateArray();
            list_files_recursively(safe_path, file_list);

            cJSON *result = cJSON_CreateObject();
            cJSON *content_arr = cJSON_CreateArray();
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "type", "text");
            char *list_str = cJSON_PrintUnformatted(file_list);
            cJSON_AddStringToObject(entry, "text", list_str);
            cJSON_AddItemToArray(content_arr, entry);
            cJSON_AddItemToObject(result, "content", content_arr);
            send_jsonrpc_response(client_socket, result, id);
            free(list_str);
            cJSON_Delete(file_list);
            cJSON_Delete(root);
            return;
        }

        // --- call_llm ---
        if (!strcmp(name, "call_llm")) {
            cJSON *prompt_item = cJSON_GetObjectItemCaseSensitive(args, "prompt");
            cJSON *model_item = cJSON_GetObjectItemCaseSensitive(args, "model");
            cJSON *tokens_item = cJSON_GetObjectItemCaseSensitive(args, "max_tokens");

            if (!cJSON_IsString(prompt_item)) {
                send_jsonrpc_error(client_socket, -32602, "Missing prompt", id);
                cJSON_Delete(root);
                return;
            }

            const char *prompt = prompt_item->valuestring;
            const char *model = model_item && cJSON_IsString(model_item) ? model_item->valuestring : "grok-4";
            int max_tokens = tokens_item && cJSON_IsNumber(tokens_item) ? tokens_item->valueint : 512;

            const char *api_key = getenv("XAI_API_KEY");
            if (!api_key || !strlen(api_key)) {
                send_jsonrpc_error(client_socket, -32000, "XAI_API_KEY not set", id);
                cJSON_Delete(root);
                return;
            }

            CURL *curl = curl_easy_init();
            if (!curl) {
                send_jsonrpc_error(client_socket, -32000, "CURL init failed", id);
                cJSON_Delete(root);
                return;
            }

            struct MemoryStruct chunk = {malloc(1), 0};

            cJSON *payload = cJSON_CreateObject();
            cJSON_AddStringToObject(payload, "model", model);
            cJSON *messages = cJSON_CreateArray();
            cJSON *msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", "user");
            cJSON_AddStringToObject(msg, "content", prompt);
            cJSON_AddItemToArray(messages, msg);
            cJSON_AddItemToObject(payload, "messages", messages);
            cJSON_AddNumberToObject(payload, "max_tokens", max_tokens);
            char *data = cJSON_PrintUnformatted(payload);
            cJSON_Delete(payload);

            curl_easy_setopt(curl, CURLOPT_URL, "https://api.x.ai/v1/chat/completions");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
            struct curl_slist *headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            char auth[256];
            snprintf(auth, sizeof(auth), "Authorization: Bearer %s", api_key);
            headers = curl_slist_append(headers, auth);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

            CURLcode res = curl_easy_perform(curl);
            free(data);

            if (res != CURLE_OK) {
                send_jsonrpc_error(client_socket, -32000, "API call failed", id);
            } else {
                cJSON *api_resp = cJSON_Parse(chunk.memory);
                const char *llm_text = "(no response)";
                if (api_resp) {
                    cJSON *choices = cJSON_GetObjectItem(api_resp, "choices");
                    if (cJSON_IsArray(choices)) {
                        cJSON *first = cJSON_GetArrayItem(choices, 0);
                        cJSON *message = cJSON_GetObjectItem(first, "message");
                        cJSON *content = cJSON_GetObjectItem(message, "content");
                        if (cJSON_IsString(content)) llm_text = content->valuestring;
                    }
                }

                cJSON *result = cJSON_CreateObject();
                cJSON *content_arr = cJSON_CreateArray();
                cJSON *entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "type", "text");
                cJSON_AddStringToObject(entry, "text", llm_text);
                cJSON_AddItemToArray(content_arr, entry);
                cJSON_AddItemToObject(result, "content", content_arr);
                send_jsonrpc_response(client_socket, result, id);
                if (api_resp) cJSON_Delete(api_resp);
            }

            free(chunk.memory);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            cJSON_Delete(root);
            return;
        }

        send_jsonrpc_error(client_socket, -32601, "Tool not found", id);
        cJSON_Delete(root);
        return;
    }

    send_jsonrpc_error(client_socket, -32601, "Method not found", id);
    cJSON_Delete(root);
}

void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    handle_request(client_socket, buffer);
    close(client_socket);
}

int main() {
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    if (listen(server_socket, 5) < 0) {
        perror("Listen failed");
        close(server_socket);
        exit(EXIT_FAILURE);
    }

    printf("MCP Server listening on port %d...\n", PORT);
    printf("Tools: read_file, recursive_file_list, call_llm (with x.ai Grok API)\n");
    printf("Set XAI_API_KEY environment variable for call_llm.\n");

    while (1) {
        client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            perror("Accept failed");
            continue;
        }
        handle_client(client_socket);
    }

    close(server_socket);
    return 0;
}