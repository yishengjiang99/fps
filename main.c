// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <curl/curl.h>

#define PORT 8080
#define BUFFER_SIZE 8192 // Increased for larger responses

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct *mem = (struct MemoryStruct *)userp;
    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (ptr == NULL) {
        return 0;
    }
    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += mem->size + realsize;
    mem->memory[mem->size] = 0;
    return realsize;
}

// Recursive file list function
void list_files_recursively(const char *base_path, char *buffer, size_t *buf_size, size_t max_size) {
    DIR *dir = opendir(base_path);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", base_path, entry->d_name);
        size_t len = strlen(path);
        if (*buf_size + len + 2 < max_size) {
            strcat(buffer, path);
            strcat(buffer, "\n");
            *buf_size += len + 1;
        }
        if (entry->d_type == DT_DIR) {
            list_files_recursively(path, buffer + *buf_size, buf_size, max_size);
        }
    }
    closedir(dir);
}

// Send JSON-RPC response
void send_jsonrpc_response(int client_socket, const char* result, int id) {
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
             "{\"jsonrpc\":\"2.0\",\"result\":%s,\"id\":%d}\n",
             result, id);
    write(client_socket, response, strlen(response));
}

// Send JSON-RPC error
void send_jsonrpc_error(int client_socket, int code, const char* message, int id) {
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
             "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":%d,\"message\":\"%s\"},\"id\":%d}\n",
             code, message, id);
    write(client_socket, response, strlen(response));
}

// Handle client request
void handle_request(int client_socket, const char* buffer) {
    char method[64] = {0};
    int id = 0;
    sscanf(buffer, "%*[^i]\"id\":%d", &id);
    sscanf(buffer, "%*[^m]\"method\":\"%[^\"]\"", method);

    if (strcmp(method, "initialize") == 0) {
        const char* capabilities = "{\"capabilities\":{\"tools\":{\"listChanged\":true}}}";
        send_jsonrpc_response(client_socket, capabilities, id);
        return;
    }

    if (strcmp(method, "tools/list") == 0) {
        const char* tools = "["
            "{\"name\":\"read_file\",\"description\":\"Read the content of a file\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
            "{\"name\":\"recursive_file_list\",\"description\":\"List files recursively in a directory\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}},"
            "{\"name\":\"call_llm\",\"description\":\"Call x.ai Grok API for chat completion\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"model\":{\"type\":\"string\",\"default\":\"grok-4\"},\"max_tokens\":{\"type\":\"number\",\"default\":512}},\"required\":[\"prompt\"]}}"
            "]";
        send_jsonrpc_response(client_socket, tools, id);
        return;
    }

    if (strcmp(method, "tools/call") == 0) {
        char name[64] = {0};
        char args[BUFFER_SIZE] = {0};
        char *args_start = strstr(buffer, "\"arguments\":");
        if (args_start) {
            sscanf(buffer, "%*[^n]\"name\":\"%[^\"]\"", name);
            // Copy arguments string
            args_start += 12; // Skip "\"arguments\":"
            char *args_end = strrchr(args_start, '}');
            if (args_end) {
                strncpy(args, args_start, args_end - args_start + 1);
                args[args_end - args_start + 1] = '\0';
            }
        } else {
            send_jsonrpc_error(client_socket, -32602, "Invalid params", id);
            return;
        }

        if (strcmp(name, "read_file") == 0) {
            char path[256] = {0};
            sscanf(args, "%*[^p]\"path\":\"%255[^\"]\"", path);
            // Security: Prefix with ./data/ to restrict access
            char safe_path[512];
            snprintf(safe_path, sizeof(safe_path), "./data/%s", path);
            FILE *file = fopen(safe_path, "r");
            if (!file) {
                send_jsonrpc_error(client_socket, -32000, "File not found", id);
                return;
            }
            char file_content[BUFFER_SIZE] = {0};
            size_t len = fread(file_content, 1, sizeof(file_content) - 1, file);
            fclose(file);
            char result[BUFFER_SIZE];
            snprintf(result, sizeof(result), "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", file_content);
            send_jsonrpc_response(client_socket, result, id);
            return;
        }

        if (strcmp(name, "recursive_file_list") == 0) {
            char path[256] = {0};
            sscanf(args, "%*[^p]\"path\":\"%255[^\"]\"", path);
            char safe_path[512];
            snprintf(safe_path, sizeof(safe_path), "./data/%s", path);
            char list_buffer[BUFFER_SIZE] = {0};
            size_t buf_size = 0;
            list_files_recursively(safe_path, list_buffer, &buf_size, sizeof(list_buffer));
            char result[BUFFER_SIZE];
            snprintf(result, sizeof(result), "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", list_buffer);
            send_jsonrpc_response(client_socket, result, id);
            return;
        }

        if (strcmp(name, "call_llm") == 0) {
            char prompt[2048] = {0};
            char model[32] = "grok-4";
            int max_tokens = 512;
            sscanf(args, "%*[^p]\"prompt\":\"%2047[^\"]\"", prompt);
            char *model_start = strstr(args, "\"model\":");
            if (model_start) sscanf(model_start, "\"model\":\"%31[^\"]\"", model);
            char *tokens_start = strstr(args, "\"max_tokens\":");
            if (tokens_start) sscanf(tokens_start, "\"max_tokens\":%d", &max_tokens);

            const char *api_key = getenv("XAI_API_KEY");
            if (!api_key || strlen(api_key) == 0) {
                send_jsonrpc_error(client_socket, -32000, "XAI_API_KEY not set", id);
                return;
            }

            CURL *curl = curl_easy_init();
            if (!curl) {
                send_jsonrpc_error(client_socket, -32000, "CURL init failed", id);
                return;
            }

            struct MemoryStruct chunk;
            chunk.memory = malloc(1);
            chunk.size = 0;

            char data[BUFFER_SIZE];
            snprintf(data, sizeof(data), "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}],\"max_tokens\":%d}", model, prompt, max_tokens);

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
            if (res != CURLE_OK) {
                send_jsonrpc_error(client_socket, -32000, "API call failed", id);
                free(chunk.memory);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                return;
            }

            // Crude parse for content
            char *content_start = strstr(chunk.memory, "\"content\":\"");
            char llm_response[2048] = {0};
            if (content_start) {
                content_start += 11;
                char *content_end = strchr(content_start, '\"');
                if (content_end) {
                    strncpy(llm_response, content_start, content_end - content_start);
                    llm_response[content_end - content_start] = '\0';
                }
            }

            char result[BUFFER_SIZE];
            snprintf(result, sizeof(result), "{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}", llm_response);

            send_jsonrpc_response(client_socket, result, id);

            free(chunk.memory);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return;
        }

        send_jsonrpc_error(client_socket, -32601, "Tool not found", id);
        return;
    }

    send_jsonrpc_error(client_socket, -32601, "Method not found", id);
}

// Handle client connection
void handle_client(int client_socket) {
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = read(client_socket, buffer, BUFFER_SIZE - 1);
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }

    handle_request(client_socket, buffer);

    // Close after one message for simplicity
    close(client_socket);
}

int main() {
    // ... (same socket setup as previous)
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
    printf("Tools: read_file, recursive_file_list, call_llm (integrated with x.ai Grok API)\n");
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