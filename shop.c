// online shop by M.Mobin.Teymourpour and A.Sattari

#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <ctype.h>
#include <float.h>
#include <time.h>

#define MAX_USERS 100
#define MAX_PRODUCTS 100
#define MAX_CATEGORIES 100
#define MAX_STORES 3
#define MAX_NAME_LENGTH 100
#define MAX_PATH_LENGTH 256
#define MAX_CONCURRENT_THREADS 10
#define THREAD_BATCH_SIZE 10
#define PASSWORD_LENGTH 20
#define RED     "\033[0;31m"
#define GREEN   "\033[0;32m"
#define YELLOW  "\033[0;33m"
#define BLUE    "\033[0;34m"
#define MAGENTA "\033[0;35m"
#define CYAN    "\033[0;36m"
#define RESET   "\033[0m"

typedef struct {
    char username[MAX_NAME_LENGTH];
    char password[PASSWORD_LENGTH];
    int numOrders;
    int isLoggedIn;
    double balance;  
    int previousStores[MAX_STORES];  
} User;

typedef struct {
    User users[MAX_USERS];
    int userCount;
    pthread_mutex_t user_mutex;
} UserDatabase;

typedef struct {
    char name[MAX_NAME_LENGTH];
    double price;
    double score;
    int entity;
    char lastModified[50];
    int found;
    int reviewCount; 
} Product;

typedef struct {
    char name[MAX_NAME_LENGTH];
    int quantity;
} ShoppingItem;

typedef struct {
    char userName[50];
    double spendingLimit;
    ShoppingItem items[MAX_PRODUCTS];
    int itemCount;
    int orderCount;  
    int firstPurchase[MAX_STORES];
} ShoppingList;

typedef struct {
    Product product;
    int storeId;
    double finalPrice;
    pthread_t threadId;
    pid_t processId;
    char category[20];
    char filePath[MAX_PATH_LENGTH];  
    int quantity;                    
} PurchaseData;

typedef struct {
    int storeId;
    Product products[MAX_PRODUCTS];
    int productCount;
    double totalPrice;
    double totalScore;
    pid_t processId;
    char productPaths[MAX_PRODUCTS][MAX_PATH_LENGTH];
    pthread_t originalThreadIds[MAX_PRODUCTS];  
} StoreList;

typedef struct {
    char productPath[MAX_PATH_LENGTH];
    ShoppingList* list;
    int storeId;
    int store_pipe_fd;
    FILE* categoryLog;
    char category[20];
    
} ThreadArg;

typedef struct {
    Product* product;
    char filepath[MAX_PATH_LENGTH];
    int quantity;
    float newScore;
    pthread_t originalThread;
} UpdateData;

typedef struct {
    StoreList* stores;
    ShoppingList* list;
    int* bestStore;
} BestStoreArg;


pthread_mutex_t pipe_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
StoreList storeLists[MAX_STORES] = {0};
sem_t *product_sem;
sem_t *file_sem;
sem_t *pipe_sem;
sem_t *category_log_sem;

void saveSuccessfulPurchase(const char* userId, int orderId, const char* productName, 
                           double price, double userScore, const char* dateBought) {
    char logPath[MAX_PATH_LENGTH];
    sprintf(logPath, "Dataset/%s_Order%d_SuccessfulPurchase.log", userId, orderId);
    
    sem_wait(file_sem);
    FILE* logFile = fopen(logPath, "a");
    if (!logFile) {
        perror("Failed to open successful purchase log");
        sem_post(file_sem);
        return;
    }
    
    fprintf(logFile, "Product: %s\n", productName);
    fprintf(logFile, "Price: %.2f\n", price);
    fprintf(logFile, "User Score: %.2f\n", userScore);
    fprintf(logFile, "Date: %s\n", dateBought);
    fprintf(logFile, "================================\n");
    
    fclose(logFile);
    sem_post(file_sem);
}

void* update_product(void* arg) {
    UpdateData* data = (UpdateData*)arg;
    sem_wait(file_sem);
    
    pthread_setschedparam(pthread_self(), SCHED_OTHER, 
                         (const struct sched_param*)&data->originalThread);
    
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char dateStr[50];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d %H:%M:%S", tm_now);
    
    FILE* file = fopen(data->filepath, "w");
    if (file) {
        float updatedScore = (data->product->score * data->product->reviewCount + data->newScore) 
                            / (data->product->reviewCount + 1);
        int newEntity = data->product->entity - data->quantity;
        int newReviewCount = data->product->reviewCount + 1;
        
        fprintf(file, "Name: %s\n", data->product->name);
        fprintf(file, "Price: %.2f\n", data->product->price);
        fprintf(file, "Score: %.2f\n", updatedScore);
        fprintf(file, "Entity: %d\n", newEntity);
        fprintf(file, "Review Count: %d\n", newReviewCount);
        fprintf(file, "Last Modified: %s\n", dateStr);
        
        fclose(file);
        
        printf("Thread %lu (original finder) updated: %s\n", 
               (unsigned long)data->originalThread, data->product->name);
        printf("New score: %.2f ((%.2f * %d + %.2f) / %d)\n", 
               updatedScore, data->product->score, data->product->reviewCount, 
               data->newScore, newReviewCount);
        printf("Remaining stock: %d (%d - %d)\n", 
               newEntity, data->product->entity, data->quantity);
        
        data->product->score = updatedScore;
        data->product->entity = newEntity;
        data->product->reviewCount = newReviewCount;
        strncpy(data->product->lastModified, dateStr, sizeof(data->product->lastModified)-1);
    }
    
    sem_post(file_sem);
    return NULL;
}

void* update_product_scores(void* arg) {
    UpdateData* data = (UpdateData*)arg;
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    char dateStr[50];
    strftime(dateStr, sizeof(dateStr), "%Y-%m-%d %H:%M:%S", tm_now);
    
    while (data && data->product) {
        printf("\nProduct: %s\n", data->product->name);
        printf("Current score: %.2f\n", data->product->score);
        printf("Enter new score (0-10): ");
        float newScore;
        scanf("%f", &newScore);
        getchar();
        
        if (newScore < 0) newScore = 0;
        if (newScore > 10) newScore = 10;
        data->newScore = newScore;

        pthread_t update_thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        
        pthread_create(&update_thread, &attr, update_product, data);
        pthread_setschedparam(update_thread, SCHED_OTHER, 
                            (const struct sched_param*)&data->originalThread);
        
        pthread_join(update_thread, NULL);
        pthread_attr_destroy(&attr);
        
        data++;
    }
    return NULL;
}

void* findBestStore(void* arg) {
    BestStoreArg* data = (BestStoreArg*)arg;
    double bestRatio = 0;
    *data->bestStore = -1;
    
    for (int i = 0; i < MAX_STORES; i++) {
        if (data->stores[i].productCount == data->list->itemCount) {
            double avgScore = data->stores[i].totalScore / data->stores[i].productCount;
            double ratio = avgScore / data->stores[i].totalPrice;
            
            printf("Store %d:\n", i + 1);
            printf("  Total Price: %.2f\n", data->stores[i].totalPrice);
            printf("  Average Score: %.2f\n", avgScore);
            printf("  Score/Price Ratio: %.6f\n", ratio);

            if (ratio > bestRatio || *data->bestStore == -1) {
                bestRatio = ratio;
                *data->bestStore = i;
            }
        }
    }
    return NULL;
}

void init_semaphores() {
    product_sem = sem_open("/product_sem", O_CREAT, 0644, 1);
    file_sem = sem_open("/file_sem", O_CREAT, 0644, 1);
    pipe_sem = sem_open("/pipe_sem", O_CREAT, 0644, 1);
    category_log_sem = sem_open("/category_log_sem", O_CREAT, 0644, 1);
}

Product readProductFile(const char *filepath) {
    Product product = {0};
    product.reviewCount = 1;  // Default to 1 review
    FILE *file = fopen(filepath, "r");
    if (file) {
        char line[256];
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\n")] = 0;

            if (strstr(line, "Name:")) {
                char* name = line + 5;
                while (*name == ' ') name++;
                strcpy(product.name, name);
            }
            else if (strstr(line, "Price:")) sscanf(line, "Price: %lf", &product.price);
            else if (strstr(line, "Score:")) sscanf(line, "Score: %lf", &product.score);
            else if (strstr(line, "Entity:")) sscanf(line, "Entity: %d", &product.entity);
            else if (strstr(line, "Review Count:")) sscanf(line, "Review Count: %d", &product.reviewCount);
            else if (strstr(line, "Last Modified:")) sscanf(line, "Last Modified: %[^\n]", product.lastModified);
        }
        fclose(file);
    }
    return product;
}

void writePurchaseLog(const char* userId, int orderId, PurchaseData* purchase) {
    char dirPath[MAX_PATH_LENGTH];
    sprintf(dirPath, "Dataset/Store%d/%s", purchase->storeId, purchase->category);

    struct stat st = {0};
    if (stat(dirPath, &st) == -1) {
        if (mkdir(dirPath, 0755) == -1) {
            perror("Failed to create directory");
            return;
        }
    }

    char logPath[MAX_PATH_LENGTH];
    sprintf(logPath, "%s/%s_Order%d.log",
            dirPath,
            userId,
            orderId);

    sem_wait(file_sem);
    FILE* logFile = fopen(logPath, "a");
    if (!logFile) {
        perror("Failed to open log file");
        sem_post(file_sem);
        return;
    }

    printf("Writing to log file: %s\n", logPath);  // Debug output

    fprintf(logFile, "=== Product Processing Information ===\n");
    fprintf(logFile, "Store: %d\n", purchase->storeId);
    fprintf(logFile, "Category: %s\n", purchase->category);
    fprintf(logFile, "Product: %s\n", purchase->product.name);
    fprintf(logFile, "Price: %.2f\n", purchase->finalPrice);
    fprintf(logFile, "Score: %.2f\n", purchase->product.score);
    fprintf(logFile, "Thread ID: %lu\n", (unsigned long)purchase->threadId);
    fprintf(logFile, "Process ID: %d\n", purchase->processId);
    fprintf(logFile, "================================\n\n");

    fflush(logFile);
    fclose(logFile);
    sem_post(file_sem);
}


void* processShoppingItem(void* arg) {
    ThreadArg* args = (ThreadArg*)arg;
    pid_t child_pid = getpid();
    pid_t parent_pid = getppid();
    
    Product product = readProductFile(args->productPath);
    product.found = 0;
    
    if (strlen(product.name) == 0) {
        printf("ERROR[PID:%d]: Failed to read product from %s\n", child_pid, args->productPath);
        return NULL;
    }
    
    pthread_mutex_lock(&log_mutex);
    fprintf(args->categoryLog, "=== Product Processing Information ===\n");
    fprintf(args->categoryLog, "Product Name: %s\n", product.name);
    fprintf(args->categoryLog, "Price: %.2f\n", product.price);
    fprintf(args->categoryLog, "Score: %.2f\n", product.score);
    fprintf(args->categoryLog, "Entity: %d\n", product.entity);
    fprintf(args->categoryLog, "Thread ID: %lu\n", (unsigned long)pthread_self());
    fprintf(args->categoryLog, "Process ID: %d\n", child_pid);
    fprintf(args->categoryLog, "Parent Process ID: %d\n", parent_pid);
    
    // Check if product is in shopping list
    for (int i = 0; i < args->list->itemCount; i++) {
        char product_name[MAX_NAME_LENGTH];
        char list_item[MAX_NAME_LENGTH];
        char *underscore;
        strncpy(product_name, product.name, MAX_NAME_LENGTH-1);
        strncpy(list_item, args->list->items[i].name, MAX_NAME_LENGTH-1);

        while ((underscore = strchr(list_item, '_')) != NULL) {
            *underscore = ' ';
        }
        
        for(int j = 0; product_name[j]; j++) product_name[j] = tolower(product_name[j]);
        for(int j = 0; list_item[j]; j++) list_item[j] = tolower(list_item[j]);
        
        if (strcmp(product_name, list_item) == 0) {
            product.found = 1;
            PurchaseData purchase = {
                .product = product,
                .storeId = args->storeId,
                .finalPrice = product.price * args->list->items[i].quantity,
                .threadId = pthread_self(),
                .processId = child_pid,
                .quantity = args->list->items[i].quantity
            };
            strncpy(purchase.category, args->category, sizeof(purchase.category)-1);
            strncpy(purchase.filePath, args->productPath, MAX_PATH_LENGTH);
                    
            pthread_mutex_lock(&pipe_mutex);
            write(args->store_pipe_fd, &purchase, sizeof(PurchaseData));
            pthread_mutex_unlock(&pipe_mutex);
            break;
        }
    }
    
    fprintf(args->categoryLog, "Found: %s\n", product.found ? "true" : "false");
    fprintf(args->categoryLog, "================================\n\n");
    fflush(args->categoryLog);
    pthread_mutex_unlock(&log_mutex);

    return NULL;
}

void processCategory(const char* categoryPath, ShoppingList* list, int storeId, int store_pipe_fd) {
    // Create log file for category
    char logPath[MAX_PATH_LENGTH];
    sprintf(logPath, "%s/%s_Order%d.log", 
            categoryPath,
            list->userName,
            list->orderCount); 

    FILE* categoryLog = fopen(logPath, "w");
    if (!categoryLog) {
        perror("Failed to create category log");
        return;
    }

    fprintf(categoryLog, "=== Category Processing Log ===\n");
    fprintf(categoryLog, "Category: %s\n", strrchr(categoryPath, '/') + 1);
    fprintf(categoryLog, "Store ID: %d\n", storeId);
    fprintf(categoryLog, "Process ID: %d\n", getpid());
    fprintf(categoryLog, "Parent Process ID: %d\n", getppid());
    fprintf(categoryLog, "User ID: %s\n", list->userName);
    fprintf(categoryLog, "Order ID: %d\n", list->orderCount);
    fprintf(categoryLog, "========================\n\n");
    fflush(categoryLog);

    struct dirent **namelist;
    int n = scandir(categoryPath, &namelist, NULL, alphasort);
    if (n < 0) {
        fclose(categoryLog);
        return;
    }

    ThreadArg thread_args[THREAD_BATCH_SIZE];
    pthread_t threads[THREAD_BATCH_SIZE];
    
    char category[20];
    const char* cat_name = strrchr(categoryPath, '/');
    if (cat_name) {
        strncpy(category, cat_name + 1, sizeof(category)-1);
    }

    int total_processed = 0;
    while (total_processed < n) {
        ThreadArg thread_args[THREAD_BATCH_SIZE];
        pthread_t threads[THREAD_BATCH_SIZE];
        int batch_size = 0;
        
        for (int i = 0; i < THREAD_BATCH_SIZE && total_processed < n; i++) {
            struct dirent *entry = namelist[total_processed];
            if (entry->d_type == DT_REG && !strstr(entry->d_name, ".log")) {
                snprintf(thread_args[batch_size].productPath, MAX_PATH_LENGTH,
                        "%s/%s", categoryPath, entry->d_name);
                thread_args[batch_size].list = list;
                thread_args[batch_size].storeId = storeId;
                thread_args[batch_size].store_pipe_fd = store_pipe_fd;
                thread_args[batch_size].categoryLog = categoryLog;
                strncpy(thread_args[batch_size].category, category, sizeof(thread_args[batch_size].category)-1);

                if (pthread_create(&threads[batch_size], NULL,
                                 processShoppingItem, &thread_args[batch_size]) == 0) {
                    batch_size++;
                }
            }
            total_processed++;
        }

        // Wait for batch completion
        for (int i = 0; i < batch_size; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    // Cleanup
    for (int i = 0; i < n; i++) {
        free(namelist[i]);
    }
    free(namelist);
    fclose(categoryLog);
}

void processUser(ShoppingList* list, int userIndex, UserDatabase* userDB) {
    char categories[][20] = {"Apparel", "Beauty", "Digital", "Food",
                             "Home", "Market", "Sports", "Toys"};

    StoreList storeLists[MAX_STORES] = {0};

    for (int storeId = 1; storeId <= MAX_STORES; storeId++) {
        int store_pipe[2];
        if (pipe(store_pipe) == -1) {
            perror("Store pipe creation failed");
            continue;
        }

        pid_t store_pid = fork();
        if (store_pid < 0) {
            perror("Store fork failed");
            continue;
        }

        if (store_pid == 0) { // Store process
        close(store_pipe[0]); // Close read end
        
        for (int i = 0; i < 8; i++) {
            pid_t category_pid = fork();
            
            if (category_pid == 0) {
                char categoryPath[MAX_PATH_LENGTH];
                sprintf(categoryPath, "Dataset/Store%d/%s", storeId, categories[i]);
                processCategory(categoryPath, list, storeId, store_pipe[1]);
                close(store_pipe[1]);
                _exit(0);
            }
        }

        // Wait for all category processes
        while (wait(NULL) > 0);
        close(store_pipe[1]);
        _exit(0);
    }

        // Parent process
        close(store_pipe[1]); // Close write end in parent

        storeLists[storeId - 1].processId = store_pid;
        storeLists[storeId - 1].storeId = storeId;

        PurchaseData purchase;
        while (read(store_pipe[0], &purchase, sizeof(PurchaseData)) > 0) {
            int idx = storeLists[storeId - 1].productCount;
            storeLists[storeId - 1].products[idx] = purchase.product;
            storeLists[storeId - 1].totalPrice += purchase.finalPrice;
            storeLists[storeId - 1].totalScore += purchase.product.score;
            storeLists[storeId - 1].originalThreadIds[idx] = purchase.threadId;  // Store the thread ID
            strncpy(storeLists[storeId - 1].productPaths[idx], purchase.filePath, MAX_PATH_LENGTH);
            storeLists[storeId - 1].productCount++;
        }

        close(store_pipe[0]);
        waitpid(store_pid, NULL, 0);
    }

    int productsFound = 0;
    for (int i = 0; i < MAX_STORES; i++) {
        if (storeLists[i].productCount > 0) {
            productsFound = 1;
            break;
        }
    }

    if (!productsFound) {
        printf("\n%s❌ No products were found matching your shopping list.%s\n", RED, RESET);
        return;
    }

    pthread_t best_store_thread;
    int bestStore = -1;
    BestStoreArg best_store_arg = {
        .stores = storeLists,
        .list = list,
        .bestStore = &bestStore
    };
    
    printf("\n%s🔍 Analyzing stores...%s\n", CYAN, RESET);
    pthread_create(&best_store_thread, NULL, findBestStore, &best_store_arg);
    pthread_join(best_store_thread, NULL);

    if (bestStore != -1) {
        double finalPrice = storeLists[bestStore].totalPrice;
    
        // Check if user has shopped in this store before
        if (userDB->users[userIndex].previousStores[bestStore]) {
            double discount = finalPrice * 0.1;
            finalPrice -= discount;
            printf("%s🎉 Previous customer discount: -%.2f (10%%)%s\n", 
                GREEN, discount, RESET);
        }
    
        printf("\n%s✨ Recommended store: Store %d%s\n", GREEN, bestStore + 1, RESET);
        printf("%s💰 Total price: %.2f%s\n", YELLOW, storeLists[bestStore].totalPrice, RESET);

        if (finalPrice > userDB->users[userIndex].balance) {
            printf("%s⚠️  Insufficient funds! Available balance: %.2f%s\n", 
                RED, userDB->users[userIndex].balance, RESET);
            return;
        }
        
        printf("\nWould you like to proceed with the purchase? (y/n): ");
        char response[10];
        fgets(response, sizeof(response), stdin);

        if (response[0] == 'y' || response[0] == 'Y') {
            pthread_mutex_lock(&userDB->user_mutex);
            userDB->users[userIndex].balance -= finalPrice;
            userDB->users[userIndex].previousStores[bestStore] = 1;  // Mark store as visited
            printf("%s💰 Remaining balance: %.2f%s\n", 
                YELLOW, userDB->users[userIndex].balance, RESET);
            pthread_mutex_unlock(&userDB->user_mutex);

            pthread_t score_thread;
            UpdateData* update_data = malloc(sizeof(UpdateData) * storeLists[bestStore].productCount);
            
            for (int i = 0; i < storeLists[bestStore].productCount; i++) {
                update_data[i].product = &storeLists[bestStore].products[i];
                strncpy(update_data[i].filepath, storeLists[bestStore].productPaths[i], MAX_PATH_LENGTH);
                update_data[i].originalThread = storeLists[bestStore].originalThreadIds[i];
                
                // Find quantity
                for (int j = 0; j < list->itemCount; j++) {
                    if (strcasecmp(list->items[j].name, update_data[i].product->name) == 0) {
                        update_data[i].quantity = list->items[j].quantity;
                        break;
                    }
                }
            }
            
            pthread_create(&score_thread, NULL, update_product_scores, update_data);
            pthread_join(score_thread, NULL);

            time_t now = time(NULL);
            struct tm *tm_now = localtime(&now);
            char dateStr[50];
            strftime(dateStr, sizeof(dateStr), "%Y-%m-%d %H:%M:%S", tm_now);

            for (int i = 0; i < storeLists[bestStore].productCount; i++) {
                Product* product = &storeLists[bestStore].products[i];
                saveSuccessfulPurchase(
                    list->userName,
                    list->orderCount,
                    product->name,
                    product->price * update_data[i].quantity,  
                    update_data[i].newScore,                   
                    dateStr
                );
            }
            free(update_data);
            
            printf("\n%s✅ Purchase completed successfully!%s\n", GREEN, RESET);
            list->orderCount++;
        } else {
            printf("%s❌ Purchase cancelled.%s\n", RED, RESET);
        }
    } else {
        printf("\nNo store has all your requested products.\n");
    }
}

void viewPreviousOrders(const char* username) {
    char command[MAX_PATH_LENGTH];
    sprintf(command, "find Dataset -name '%s_Order*_SuccessfulPurchase.log' -type f", username);
    
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        printf("Error searching for order logs!\n");
        return;
    }
    
    char path[MAX_PATH_LENGTH];
    int found = 0;
    
    while (fgets(path, sizeof(path), pipe)) {
        path[strcspn(path, "\n")] = 0;
        FILE* log = fopen(path, "r");
        if (log) {
            char line[256];
            char product_name[MAX_NAME_LENGTH] = {0};
            double price = 0;
            double user_score = 0;
            char date[50] = {0};
            int has_products = 0;
            
            printf("\n%s=== 📦 Order from %s ===%s\n", CYAN, path, RESET);
            
            while (fgets(line, sizeof(line), log)) {
                if (strstr(line, "Product:")) {
                    sscanf(line, "Product: %[^\n]", product_name);
                } else if (strstr(line, "Price:")) {
                    sscanf(line, "Price: %lf", &price);
                } else if (strstr(line, "User Score:")) {
                    sscanf(line, "User Score: %lf", &user_score);
                } else if (strstr(line, "Date:")) {
                    sscanf(line, "Date: %[^\n]", date);
                    if (strlen(product_name) > 0) {
                        printf("\n%s📝 Name: %s%s\n", YELLOW, product_name, RESET);
                        printf("%s💰 Price: %.2f%s\n", GREEN, price, RESET);
                        printf("%s⭐ User Score: %.2f%s\n", MAGENTA, user_score, RESET);
                        printf("%s🕒 Date Bought: %s%s\n", BLUE, date, RESET);
                        has_products = 1;
                    }
                }
            }
            
            if (has_products) {
                found = 1;
            }
            
            fclose(log);
        }
    }
    
    if (!found) {
        printf("No previous orders found.\n");
    }
    pclose(pipe);
}

int handleUserSession(const char* username, int userIndex, int shmid) {
    UserDatabase* userDB = (UserDatabase*)shmat(shmid, NULL, 0);
    if (userDB == (void*)-1) return 1;

    while(1) {
        printf("\n%s=== 👤 Logged in as %s (Balance: %.2f) ===%s\n", 
                CYAN, username, userDB->users[userIndex].balance, RESET);
        printf("%s1. 📋 View previous orders%s\n", YELLOW, RESET);
        printf("%s2. 🛒 New order%s\n", YELLOW, RESET);
        printf("%s3. 💰 Add balance%s\n", YELLOW, RESET);
        printf("%s4. 🚪 Logout%s\n", YELLOW, RESET);
        
        int choice;
        scanf("%d", &choice);
        getchar();
        
        switch(choice) {
            case 1:
                viewPreviousOrders(username);
                break;
            case 2: {
                ShoppingList list = {0};
                strncpy(list.userName, username, sizeof(list.userName) - 1);
                list.orderCount = userDB->users[userIndex].numOrders + 1;
                
                printf("Orderlist%d:\n", list.orderCount);
                list.itemCount = 0;
                
                char buffer[256];
                while (1) {
                    if (fgets(buffer, sizeof(buffer), stdin) == NULL || buffer[0] == '\n') break;
                    
                    char name[MAX_NAME_LENGTH];
                    int quantity;
                    if (sscanf(buffer, "%s %d", name, &quantity) == 2) {
                        strncpy(list.items[list.itemCount].name, name, MAX_NAME_LENGTH - 1);
                        list.items[list.itemCount].quantity = quantity;
                        list.itemCount++;
                    }
                }
                
                processUser(&list, userIndex, userDB);
                pthread_mutex_lock(&userDB->user_mutex);
                userDB->users[userIndex].numOrders++;
                pthread_mutex_unlock(&userDB->user_mutex);
                break;
            }
            case 3: {
                printf("Enter amount to add: ");
                char amountStr[50];
                fgets(amountStr, sizeof(amountStr), stdin);
                double amount = atof(amountStr);
                
                pthread_mutex_lock(&userDB->user_mutex);
                userDB->users[userIndex].balance += amount;
                printf("%s✅ Added %.2f. New balance: %.2f%s\n", 
                    GREEN, amount, userDB->users[userIndex].balance, RESET);
                pthread_mutex_unlock(&userDB->user_mutex);
                break;
            }
            case 4:
                pthread_mutex_lock(&userDB->user_mutex);
                userDB->users[userIndex].isLoggedIn = 0;
                pthread_mutex_unlock(&userDB->user_mutex);
                shmdt(userDB);
                return 0;
            default:
                printf("Invalid option!\n");
        }
    }
}

int main(int argc, char *argv[]) {
    init_semaphores();

    // Check if program was launched with user session arguments
    if (argc == 4) {
        // argv[1] = username, argv[2] = userIndex, argv[3] = shmid
        return handleUserSession(argv[1], atoi(argv[2]), atoi(argv[3]));
    }

    // Normal startup - Create shared memory for user database
    key_t key = ftok("/tmp", 'u');  // Change from "." to "/tmp" for more reliable key
    int shmid = shmget(key, sizeof(UserDatabase), 0666 | IPC_CREAT);
    if (shmid < 0) {
        perror("shmget failed");
        exit(1);
    }

    UserDatabase* userDB = (UserDatabase*)shmat(shmid, NULL, 0);
    if (userDB == (void*)-1) {
        perror("shmat failed");
        exit(1);
    }

    // Initialize mutex for user database
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&userDB->user_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    while(1) {
        printf("\n%s=== 🛍️  Welcome to Online Shop 🛍️  ===%s\n", CYAN, RESET);
        printf("%s1. 👤 Create new user%s\n", YELLOW, RESET);
        printf("%s2. 🔑 Login%s\n", YELLOW, RESET);
        printf("%s3. 🚪 Exit%s\n", YELLOW, RESET);
        printf("%s💭 Choose option: %s", GREEN, RESET);
        
        int choice;
        scanf("%d", &choice);
        getchar();

        switch(choice) {
            case 1: {
                char username[MAX_NAME_LENGTH];
                char password[PASSWORD_LENGTH];
                char balanceStr[50];
                
                printf("Enter username: ");
                fgets(username, sizeof(username), stdin);
                username[strcspn(username, "\n")] = 0;
                
                printf("Enter password: ");
                fgets(password, sizeof(password), stdin);
                password[strcspn(password, "\n")] = 0;

                printf("Enter initial balance: ");
                fgets(balanceStr, sizeof(balanceStr), stdin);
                balanceStr[strcspn(balanceStr, "\n")] = 0;
                double initialBalance = atof(balanceStr);
                
                pthread_mutex_lock(&userDB->user_mutex);
                int exists = 0;
                for (int i = 0; i < userDB->userCount; i++) {
                    if (strcmp(userDB->users[i].username, username) == 0) {
                        exists = 1;
                        break;
                    }
                }
                
                if (!exists && userDB->userCount < MAX_USERS) {
                    strcpy(userDB->users[userDB->userCount].username, username);
                    strcpy(userDB->users[userDB->userCount].password, password);
                    userDB->users[userDB->userCount].numOrders = 0;
                    userDB->users[userDB->userCount].isLoggedIn = 0;
                    userDB->users[userDB->userCount].balance = initialBalance;
                    memset(userDB->users[userDB->userCount].previousStores, 0, sizeof(int) * MAX_STORES);
                    userDB->userCount++;
                    printf("%s✅ User created successfully!%s\n", GREEN, RESET);
                } else {
                    printf("%s⚠️  Username already exists or max users reached!%s\n", RED, RESET);
                }
                pthread_mutex_unlock(&userDB->user_mutex);
                break;
            }
            case 2: {
                char username[MAX_NAME_LENGTH];
                char password[PASSWORD_LENGTH];
                
                printf("Enter username: ");
                fgets(username, sizeof(username), stdin);
                username[strcspn(username, "\n")] = 0;
                
                printf("Enter password: ");
                fgets(password, sizeof(password), stdin);
                password[strcspn(password, "\n")] = 0;
                
                pthread_mutex_lock(&userDB->user_mutex);
                int userIndex = -1;
                for (int i = 0; i < userDB->userCount; i++) {
                    if (strcmp(userDB->users[i].username, username) == 0 &&
                        strcmp(userDB->users[i].password, password) == 0) {
                        userIndex = i;
                        break;
                    }
                }
                
                if (userIndex >= 0) {
                    if (userDB->users[userIndex].isLoggedIn) {
                        printf("%s⚠️  User already logged in!%s\n", RED, RESET);
                        pthread_mutex_unlock(&userDB->user_mutex);
                        break;
                    }
                    
                    userDB->users[userIndex].isLoggedIn = 1;
                    pthread_mutex_unlock(&userDB->user_mutex);
                    
                    // Open new terminal for user session
                    char command[512];
                    #ifdef __APPLE__
                        // iTerm2 command for macOS
                        sprintf(command, "osascript -e 'tell application \"iTerm\"\n"
                                "    create window with default profile\n"
                                "    tell current session of current window\n"
                                "        write text \"cd \\\"%s\\\" && ./shop %s %d %d\"\n"
                                "    end tell\n"
                                "end tell'",
                                getcwd(NULL, 0), username, userIndex, shmid);
                    #elif defined(_WIN32)
                        // Windows Terminal command
                        sprintf(command, "start wt.exe -d \"%s\" ./shop %s %d %d",
                                getcwd(NULL, 0), username, userIndex, shmid);
                    #else
                        // GNOME Terminal for Linux
                        sprintf(command, "gnome-terminal -- bash -c 'cd \"%s\" && ./shop %s %d %d; exec bash'",
                                getcwd(NULL, 0), username, userIndex, shmid);
                    #endif
                    system(command);
                } else {
                    printf("%s❌ Invalid username or password!%s\n", RED, RESET);
                    pthread_mutex_unlock(&userDB->user_mutex);
                }
                break;
            }
            case 3:
                shmdt(userDB);
                shmctl(shmid, IPC_RMID, NULL);
                return 0;
            default:
                printf("Invalid option!\n");
        }
    }
}