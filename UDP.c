#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>

#define DEVICE_PATH_HUMIDITY "/sys/class/gpio_class_humidity/gpio_char_device_humidity/"
#define DEVICE_PATH_SALTINESS "/sys/class/gpio_class_saltiness/gpio_char_device_salt/"
#define DEVICE_PATH_LIGHT "/sys/class/gpio_class_light/gpio_char_device_light/"

#define MAX_CHANGES 10
#define MONITOR_DURATION 10 // 10 seconds
#define UDP_SERVER_IP "192.168.5.5"
#define UDP_SERVER_PORT 50007

typedef struct {
    char *device_path;
    char *log_file;
    char *sensor_name;
    char *changes[MAX_CHANGES];
    int change_count;
    int monitoring; // Flag to control monitoring
} Device;



void delete_specific_files() {
    // List of files to be deleted
    const char *files_to_delete[] = {
        "humidity_log.txt",
        "light_log.txt",
        "saltiness_log.txt",
        "received_data_1.txt",
        "received_data_2.txt",
        "received_data_3.txt"        
    };

    // Number of files to delete
    size_t num_files = sizeof(files_to_delete) / sizeof(files_to_delete[0]);

    // Loop through the list and attempt to delete each file
    for (size_t i = 0; i < num_files; i++) {
        if (remove(files_to_delete[i]) == 0) {
            printf("Successfully deleted: %s\n", files_to_delete[i]);
        } else {
            // Check if the error is because the file does not exist
            if (errno == ENOENT) {
                printf("File does not exist: %s\n", files_to_delete[i]);
            } else {
                fprintf(stderr, "Error deleting file %s: %s\n", files_to_delete[i], strerror(errno));
            }
        }
    }
}
void read_device(Device *device) {
    char value_path[256], changed_value_path[256];
    snprintf(value_path, sizeof(value_path), "%svalue", device->device_path);
    snprintf(changed_value_path, sizeof(changed_value_path), "%schanged_value", device->device_path);

    FILE *value_file = fopen(value_path, "r");
    if (!value_file) {
        fprintf(stderr, "Error: Failed to open value file for %s\n", device->sensor_name);
        return;
    }

    FILE *changed_value_file = fopen(changed_value_path, "r");
    if (!changed_value_file) {
        fprintf(stderr, "Error: Failed to open changed_value file for %s\n", device->sensor_name);
        fclose(value_file);
        return;
    }

    char value[256], changed_value[256];
    fgets(value, sizeof(value), value_file);
    fgets(changed_value, sizeof(changed_value), changed_value_file);

    if (strcmp(changed_value, "1\n") == 0) {
        if (device->change_count == MAX_CHANGES) {
            free(device->changes[0]);
            memmove(device->changes, device->changes + 1, (MAX_CHANGES - 1) * sizeof(char *));
            device->change_count--;
        }
        device->changes[device->change_count] = strdup(value);
        device->change_count++;

        // Print the new value with the sensor name
        printf("New value for %s: %s", device->sensor_name, value);

        // Reset the changed_value to 0
        fclose(changed_value_file);
        changed_value_file = fopen(changed_value_path, "w");
        if (!changed_value_file) {
            fprintf(stderr, "Error: Failed to open changed_value file for writing for %s\n", device->sensor_name);
        } else {
            fputs("0", changed_value_file);
            fclose(changed_value_file);
        }
    } else {
        fclose(changed_value_file);
    }

    fclose(value_file);
}

void write_log(Device *device) {
    FILE *log_file = fopen(device->log_file, "w"); // Open in write mode to overwrite
    if (!log_file) {
        fprintf(stderr, "Error: Failed to open log file for %s\n", device->sensor_name);
        return;
    }
    for (int i = 0; i < device->change_count; i++) {
        fprintf(log_file, "%s", device->changes[i]);
    }
    fclose(log_file);
}

void *monitor_device(void *arg) {
    Device *device = (Device *)arg;
    time_t start_time = time(NULL);

    // Monitor for a specified duration
    device->monitoring = 1; // Set monitoring flag
    while (device->monitoring && (time(NULL) - start_time < MONITOR_DURATION)) {
        read_device(device);
        write_log(device);
        sleep(0.1);
    }
    
    // After monitoring ends, mark monitoring as stopped
    device->monitoring = 0;
    return NULL;
}

float calculate_similarity(const char *file_path1, const char *file_path2) {
    FILE *file1 = fopen(file_path1, "r");
    FILE *file2 = fopen(file_path2, "r");

    if (!file1) {
        fprintf(stderr, "Error: Failed to open the first data file: %s\n", file_path1);
        return 0.0;
    }
    
    if (!file2) {
        fprintf(stderr, "Error: Failed to open the second data file: %s\n", file_path2);
        fclose(file1); // Close the first file before returning
        return 0.0;
    }

    char line1[256];
    char line2[256];
    int matches = 0;
    int total_lines_file1 = 0;
    int total_lines_file2 = 0;

    // Read each line from the first file and compare with lines from the second file
    while (fgets(line1, sizeof(line1), file1)) {
        total_lines_file1++;
        // Compare with each line in the second file
        fseek(file2, 0, SEEK_SET); // Reset file2 pointer to the beginning
        while (fgets(line2, sizeof(line2), file2)) {
            total_lines_file2++;
            if (strcmp(line1, line2) == 0) {
                matches++;
                break; // Break after finding a match
            }
        }
    }

    fclose(file1);
    fclose(file2);

    // Calculate percentage based on the lines from file1
    return (total_lines_file1 > 0) ? ((float)matches / total_lines_file1) * 100.0 : 0.0; // Calculate percentage
}

void save_received_data(const char *data, int index) {
    char filename[20];
    snprintf(filename, sizeof(filename), "received_data_%d.txt", index + 1); // Create filename for saving data

    FILE *file = fopen(filename, "a"); // Open in append mode
    if (!file) {
        fprintf(stderr, "Error: Failed to open file %s for writing\n", filename);
        return;
    }

	for (size_t i = 0; i < strlen(data); i++) {
		fprintf(file, "%u\n", (uint8_t)data[i]); // Cast character to uint8_t and write
	}
    fclose(file);
}

int main() {
    Device devices[] = {
        {DEVICE_PATH_HUMIDITY, "humidity_log.txt", "Humidity", {0}, 0, 0},
        {DEVICE_PATH_SALTINESS, "saltiness_log.txt", "Saltiness", {0}, 0, 0},
        {DEVICE_PATH_LIGHT, "light_log.txt", "Light", {0}, 0, 0}
    };

    int device_count = sizeof(devices) / sizeof(devices[0]);
    pthread_t threads[device_count];

    // Create UDP socket
    int sockfd;
    struct sockaddr_in server_addr, from_addr;
    socklen_t addr_len = sizeof(from_addr);
    char buffer[256]; // Increased buffer size for incoming data

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        return EXIT_FAILURE;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(UDP_SERVER_PORT);
    inet_pton(AF_INET, UDP_SERVER_IP, &server_addr.sin_addr);

    while (1) {
        printf("- press 1 to send a UDP message and start monitoring for 10 seconds,\n\r- press 2 to compare received data,\n\r- press 3 to PUMP state\n\r- press any other key to exit...\n\r");
        int input = getchar(); // Get user input
        getchar(); // Consume the newline character

        if (input == '1') {
			delete_specific_files();
            // Send UDP message
            const char *message = "1"; // Message to send
            sendto(sockfd, message, strlen(message), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));

            // Receive response immediately
            int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&from_addr, &addr_len);
            if (bytes_received > 0) {
                buffer[bytes_received] = '\0'; // Null-terminate the received string
                printf("Received response: %s\n", buffer); // Print the response

                if (strcmp(buffer, "1") == 0) {
                    // Start monitoring if the response is "1"
                    for (int i = 0; i < device_count; i++) {
                        pthread_create(&threads[i], NULL, monitor_device, &devices[i]);
                    }

                    // Wait for all threads to finish monitoring
                    for (int i = 0; i < device_count; i++) {
                        pthread_join(threads[i], NULL);
                    }

                    printf("Monitoring complete. Logs updated.\n");
                } else {
                    printf("Received unexpected response: %s\n", buffer);
                }
            } else {
                printf("Failed to receive response.\n");
            }
        } else if (input == '2') {
    // Send UDP request to receive data
    const char *request_message = "request_data"; // Message to send for data
    sendto(sockfd, request_message, strlen(request_message), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));
    int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&from_addr, &addr_len);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0'; // Null-terminate the received string
        printf("Received response: %s\n", buffer); // Print the response
    }

    // Receive incoming data and save to three files
    int received_count = 0;

    while (received_count < 3) {
        int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&from_addr, &addr_len);
        if (bytes_received > 0) {
            buffer[bytes_received] = '\0'; // Null-terminate the received string
            save_received_data(buffer, received_count); // Save received data to file
            received_count++;
        } else {
            printf("Failed to receive data.\n");
        }
    }

    // Calculate and output the similarity percentage for each log file
    for (int i = 0; i < 3; i++) {
		char file_path[50];
		sprintf(file_path, "received_data_%d.txt", i + 1); 
        float similarity = calculate_similarity(file_path, devices[i].log_file);
        printf("Similarity percentage for %s: %.2f%%\n", devices[i].log_file, similarity);
    }
} 
else if (input == '3') {
            // Send '3' and receive the same value back twice
            const char *message = "3"; // Message to send
            sendto(sockfd, message, strlen(message), 0, (const struct sockaddr *)&server_addr, sizeof(server_addr));

            for (int i = 0; i < 2; i++) { // Receive the response twice
                int bytes_received = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0, (struct sockaddr *)&from_addr, &addr_len);
                if (bytes_received > 0) {
                    buffer[bytes_received] = '\0'; // Null-terminate the received string
                    printf("%s\n", buffer); // Print the received value
                } else {
                    printf("Failed to receive response for '3'.\n");
                }
            }
        }
     else {
            printf("Exiting...\n");
            break; // Exit the loop and the program
        }
    }

    close(sockfd); // Close the socket
    return 0;
}
