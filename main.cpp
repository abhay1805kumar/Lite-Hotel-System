#include <sqlite3.h>
#include <bits/stdc++.h>

// ======================= Database layer =======================

using Param = std::variant<long long, std::string>;

// NULL-safe row wrapper: managers never see char** rows
class Row {
    sqlite3_stmt* stmt;
public:
    explicit Row(sqlite3_stmt* s) : stmt(s) {}
    int getInt(int col) const { return sqlite3_column_int(stmt, col); }  // NULL -> 0
    std::string getText(int col) const {
        const unsigned char* t = sqlite3_column_text(stmt, col);
        return t ? reinterpret_cast<const char*>(t) : "";                // NULL -> ""
    }
};

class Database {
    sqlite3* db = nullptr;
    Database() = default;

    // RAII prepared statement
    class Statement {
        sqlite3_stmt* stmt = nullptr;
    public:
        Statement(sqlite3* db, const std::string& sql, const std::vector<Param>& params) {
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(db));
            for (size_t i = 0; i < params.size(); ++i) {
                int idx = static_cast<int>(i) + 1;
                int rc = std::visit([&](const auto& v) -> int {
                    using V = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<V, long long>)
                        return sqlite3_bind_int64(stmt, idx, v);
                    else
                        return sqlite3_bind_text(stmt, idx, v.c_str(), -1, SQLITE_TRANSIENT);
                }, params[i]);
                if (rc != SQLITE_OK) {
                    std::string msg = sqlite3_errmsg(db);
                    sqlite3_finalize(stmt);
                    throw std::runtime_error(msg);
                }
            }
        }
        ~Statement() { sqlite3_finalize(stmt); }
        Statement(const Statement&) = delete;
        Statement& operator=(const Statement&) = delete;
        sqlite3_stmt* get() const { return stmt; }
    };

public:
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database() { close(); }

    // Meyers singleton: thread-safe init 
    static Database& getInstance() {
        static Database instance;
        return instance;
    }

    bool connect(const std::string& name = "hotel.db") {
        if (sqlite3_open(name.c_str(), &db) != SQLITE_OK) {
            std::cerr << "Cannot open DB: " << sqlite3_errmsg(db) << "\n";
            return false;
        }
        sqlite3_busy_timeout(db, 3000);  
        try {
            execRaw("PRAGMA foreign_keys = ON");
            initializeTables();
        } catch (const std::exception& e) {
            std::cerr << "Init failed: " << e.what() << "\n";
            return false;
        }
        return true;
    }

    void close() {
        if (db) { sqlite3_close(db); db = nullptr; }
    }

    // Plain SQL without params (DDL, BEGIN/COMMIT)
    void execRaw(const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "unknown error";
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
    }

    // INSERT / UPDATE / DELETE with bound params
    void execute(const std::string& sql, const std::vector<Param>& params = {}) {
        Statement st(db, sql, params);
        int rc;
        while ((rc = sqlite3_step(st.get())) == SQLITE_ROW) {}
        if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
    }

    int changes() const { return sqlite3_changes(db); }

    // SELECT -> vector<T>; the std::function mapper turns each row into a C++ object
    template <typename T>
    std::vector<T> query(const std::string& sql, const std::vector<Param>& params,
                         const std::function<T(const Row&)>& mapper) {
        Statement st(db, sql, params);
        std::vector<T> out;
        int rc;
        while ((rc = sqlite3_step(st.get())) == SQLITE_ROW)
            out.push_back(mapper(Row(st.get())));
        if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        return out;
    }

private:
    void initializeTables() {
        execRaw("CREATE TABLE IF NOT EXISTS users ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "username TEXT UNIQUE NOT NULL,"
                "password TEXT NOT NULL,"
                "role TEXT NOT NULL,"
                "created_at DATETIME DEFAULT (datetime('now','localtime')))");

        execRaw("CREATE TABLE IF NOT EXISTS inventory ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "name TEXT UNIQUE NOT NULL,"
                "price INTEGER NOT NULL CHECK(price >= 0),"
                "quantity INTEGER NOT NULL CHECK(quantity >= 0),"
                "category TEXT NOT NULL)");

        execRaw("CREATE TABLE IF NOT EXISTS sales ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "item_id INTEGER NOT NULL,"
                "quantity INTEGER NOT NULL,"
                "total_price INTEGER NOT NULL,"
                "user_id INTEGER NOT NULL,"
                "timestamp DATETIME DEFAULT (datetime('now','localtime')),"
                "FOREIGN KEY (item_id) REFERENCES inventory(id),"
                "FOREIGN KEY (user_id) REFERENCES users(id))");

        execRaw("CREATE INDEX IF NOT EXISTS idx_sales_ts ON sales(timestamp)");

        execRaw("INSERT OR IGNORE INTO inventory (name, price, quantity, category) VALUES "
                "('Room',1200,10,'accommodation'),"
                "('Pasta',250,50,'food'),"
                "('Burger',120,50,'food'),"
                "('Noodles',140,50,'food'),"
                "('Shake',120,50,'drink'),"
                "('Chicken Roll',150,50,'food')");
    }
};

// RAII transaction: scope se bina commit nikle to automatic ROLLBACK
class Transaction {
    Database& db;
    bool finished = false;
public:
    explicit Transaction(Database& d) : db(d) { db.execRaw("BEGIN IMMEDIATE"); }
    void commit() { db.execRaw("COMMIT"); finished = true; }
    ~Transaction() {
        if (!finished) {
            try { db.execRaw("ROLLBACK"); } catch (...) {}  // destructor se throw nahi
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
};

// ======================= Models =======================

struct Item {
    int id = 0;
    std::string name;
    int price = 0;
    std::string category;
    int quantity = 0;
};

struct SaleSummary {
    std::string name;
    int qtySold = 0;
    int revenue = 0;
};

struct OrderResult {
    bool ok = false;
    std::string message;
    Item item;
    int quantity = 0;
    int total = 0;
};

// ======================= Managers =======================

class InventoryManager {
    static Item mapItem(const Row& r) {
        return Item{r.getInt(0), r.getText(1), r.getInt(2), r.getText(3), r.getInt(4)};
    }
public:
    static std::vector<Item> getAllItems() {
        return Database::getInstance().query<Item>(
            "SELECT id,name,price,category,quantity FROM inventory ORDER BY category,name",
            {}, mapItem);
    }

    static std::optional<Item> getItemById(int id) {
        auto v = Database::getInstance().query<Item>(
            "SELECT id,name,price,category,quantity FROM inventory WHERE id = ?",
            {static_cast<long long>(id)}, mapItem);
        if (v.empty()) return std::nullopt;
        return v.front();
    }

    // Atomic check + decrease in a single statement
    static bool tryDecrease(int itemId, int amount) {
        auto& db = Database::getInstance();
        db.execute("UPDATE inventory SET quantity = quantity - ? WHERE id = ? AND quantity >= ?",
                   {static_cast<long long>(amount), static_cast<long long>(itemId),
                    static_cast<long long>(amount)});
        return db.changes() == 1;
    }
};

class OrderManager {
public:
    // Business logic only; printing is done by HotelApp
    static OrderResult processOrder(int itemId, int quantity, int userId) {
        auto& db = Database::getInstance();
        try {
            Transaction tx(db);  // exception / early return -> auto ROLLBACK
            auto item = InventoryManager::getItemById(itemId);
            if (!item) return {false, "Item not found", {}, 0, 0};

            if (!InventoryManager::tryDecrease(itemId, quantity))
                return {false, "Not enough stock", *item, quantity, 0};

            int total = item->price * quantity;
            db.execute("INSERT INTO sales (item_id, quantity, total_price, user_id) "
                       "VALUES (?,?,?,?)",
                       {static_cast<long long>(itemId), static_cast<long long>(quantity),
                        static_cast<long long>(total), static_cast<long long>(userId)});
            tx.commit();
            return {true, "OK", *item, quantity, total};
        } catch (const std::exception& e) {
            return {false, e.what(), {}, 0, 0};
        }
    }
};

class UserManager {
public:
    static std::string hashPassword(const std::string& pw) {
        char out[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(out, pw.c_str(), pw.size(),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
            throw std::runtime_error("password hashing failed");
        return out;
    }

    static int authenticate(const std::string& user, const std::string& pw) {
        using Rec = std::pair<int, std::string>;
        auto rows = Database::getInstance().query<Rec>(
            "SELECT id, password FROM users WHERE username = ?", {user},
            [](const Row& r) { return Rec{r.getInt(0), r.getText(1)}; });
        if (rows.empty()) return -1;
        return crypto_pwhash_str_verify(rows[0].second.c_str(), pw.c_str(), pw.size()) == 0
                   ? rows[0].first : -1;
    }

    static std::string getUserRole(int id) {
        auto v = Database::getInstance().query<std::string>(
            "SELECT role FROM users WHERE id = ?", {static_cast<long long>(id)},
            [](const Row& r) { return r.getText(0); });
        return v.empty() ? "" : v[0];
    }

    static bool addUser(const std::string& user, const std::string& pw, const std::string& role) {
        try {
            Database::getInstance().execute(
                "INSERT INTO users (username,password,role) VALUES (?,?,?)",
                {user, hashPassword(pw), role});
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    // First run only: default admin with a hashed password
    static void ensureDefaultAdmin() {
        auto c = Database::getInstance().query<int>(
            "SELECT COUNT(*) FROM users", {},
            [](const Row& r) { return r.getInt(0); });
        if (!c.empty() && c[0] == 0) addUser("admin", "admin123", "admin");
    }
};

class ReportManager {
    static std::string csvEscape(const std::string& s) {
        if (s.find_first_of(",\"\n") == std::string::npos) return s;
        std::string o = "\"";
        for (char ch : s) {
            if (ch == '"') o += '"';
            o += ch;
        }
        return o + "\"";
    }
public:
    static std::vector<SaleSummary> getDailySales() {
        
        return Database::getInstance().query<SaleSummary>(
            "SELECT i.name, SUM(s.quantity), SUM(s.total_price) FROM sales s "
            "JOIN inventory i ON s.item_id = i.id "
            "WHERE s.timestamp >= date('now','localtime') "
            "AND s.timestamp < date('now','localtime','+1 day') "
            "GROUP BY s.item_id ORDER BY i.category, i.name", {},
            [](const Row& r) {
                return SaleSummary{r.getText(0), r.getInt(1), r.getInt(2)};
            });
    }

    static bool exportSalesReport(const std::string& filename) {
        std::ofstream f(filename);
        if (!f) return false;
        f << "Date,Item,Category,Quantity,Unit Price,Total Price,User\n";

        using Line = std::vector<std::string>;
        auto rows = Database::getInstance().query<Line>(
            "SELECT s.timestamp,i.name,i.category,s.quantity,i.price,s.total_price,u.username "
            "FROM sales s JOIN inventory i ON s.item_id=i.id JOIN users u ON s.user_id=u.id "
            "WHERE s.timestamp >= date('now','localtime') "
            "AND s.timestamp < date('now','localtime','+1 day') ORDER BY s.timestamp", {},
            [](const Row& r) {
                Line l;
                for (int i = 0; i < 7; ++i) l.push_back(r.getText(i));
                return l;
            });

        for (const auto& l : rows)
            for (size_t i = 0; i < l.size(); ++i)
                f << csvEscape(l[i]) << (i + 1 < l.size() ? "," : "\n");
        return true;
    }
};

// ======================= Application (UI layer) =======================

class HotelApp {
    int currentUserId = -1;
    std::string currentUserRole;

    static std::string readLine() {
        std::string s;
        std::getline(std::cin, s);
        return s;
    }

    static bool readInt(int& out) {
        std::string s = readLine();
        try {
            size_t pos = 0;
            out = std::stoi(s, &pos);
            return pos == s.size();
        } catch (...) {
            return false;
        }
    }

    bool isAdmin() const { return currentUserRole == "admin"; }

public:
    bool initialize() {
        std::cout << "\n=================================================";
        std::cout << "\n|            HOTEL MANAGEMENT SYSTEM            |";
        std::cout << "\n=================================================";

        if (!Database::getInstance().connect()) {
            std::cerr << "Failed to initialize database!\n";
            return false;
        }
        UserManager::ensureDefaultAdmin();
        return login();
    }

    void run() {
        while (true) {
            displayMenu();
            int choice = -1;
            if (!readInt(choice)) choice = -1;

            if (processMenuChoice(choice)) break;

            std::cout << "\n\nPress Enter to continue...";
            readLine();
        }
    }

private:
    bool login() {
        for (int attempts = 1; attempts <= 3; ++attempts) {
            std::cout << "\n\n=== LOGIN ===\nUsername: ";
            std::string username = readLine();
            std::cout << "Password: ";
            std::string password = readLine();

            currentUserId = UserManager::authenticate(username, password);
            if (currentUserId > 0) {
                currentUserRole = UserManager::getUserRole(currentUserId);
                std::cout << "\nLogin successful! Welcome, " << username << "!\n";
                return true;
            }
            std::cout << "\nInvalid username or password. Attempts remaining: "
                      << (3 - attempts);
        }
        std::cout << "\nToo many failed attempts. Exiting...\n";
        return false;
    }

    void displayMenu() {
        auto items = InventoryManager::getAllItems();
        std::cout << "\n\n\t Please select from the menu options";
        int n = 1;
        for (const auto& it : items)
            std::cout << "\n" << n++ << ") " << it.name << " - Rs " << it.price;

        std::cout << "\n" << n++ << ") View today's sales report";
        std::cout << "\n" << n++ << ") View inventory status";
        if (isAdmin()) {
            std::cout << "\n" << n++ << ") Archive today's sales (CSV)";
            std::cout << "\n" << n++ << ") Add new user";
        }
        std::cout << "\n" << n << ") Exit";
        std::cout << "\n\nPlease enter your choice: ";
    }

    bool processMenuChoice(int choice) {
        auto items = InventoryManager::getAllItems();
        int count = static_cast<int>(items.size());

        // Item purchase
        if (choice >= 1 && choice <= count) {
            const Item& it = items[choice - 1];
            std::cout << "\nEnter " << it.name << " quantity: ";
            int qty = 0;
            if (!readInt(qty) || qty <= 0 || qty > 100000) {
                std::cout << "\nInvalid quantity!";
                return false;
            }
            printOrder(OrderManager::processOrder(it.id, qty, currentUserId));
            return false;
        }

        int opt = choice - count;  // 1 = sales report, 2 = inventory, ...
        int exitOpt = isAdmin() ? 5 : 3;

        if (opt == 1)      showDailySales();
        else if (opt == 2) showInventory();
        else if (isAdmin() && opt == 3) archiveSales();
        else if (isAdmin() && opt == 4) addNewUser();
        else if (opt == exitOpt) {
            std::cout << "\nExiting program...\n";
            return true;
        } else {
            std::cout << "\nPlease select a valid option!";
        }
        return false;
    }

    void printOrder(const OrderResult& r) {
        if (!r.ok) {
            std::cout << "\nOrder failed: " << r.message;
            return;
        }
        std::cout << "\n\n\t" << r.quantity << " x " << r.item.name;
        std::cout << (r.item.category == "accommodation" ? " allotted to you!" : " ordered!");
        std::cout << "\n\n Bill details:";
        std::cout << "\n Item: " << r.item.name;
        std::cout << "\n Quantity: " << r.quantity;
        std::cout << "\n Price per item: Rs " << r.item.price;
        std::cout << "\n Total: Rs " << r.total << "\n";
    }

    void showDailySales() {
        auto rows = ReportManager::getDailySales();
        std::cout << "\n\tToday's Sales and Collection\n";
        std::cout << "\n=======================================================";
        std::cout << "\n" << std::left << std::setw(22) << "Item"
                  << std::setw(16) << "Quantity Sold" << "Revenue (Rs)";
        std::cout << "\n=====================================================";
        int total = 0;
        for (const auto& s : rows) {
            std::cout << "\n" << std::left << std::setw(22) << s.name
                      << std::setw(16) << s.qtySold << s.revenue;
            total += s.revenue;
        }
        std::cout << "\n=================================================";
        std::cout << "\nTotal Revenue: Rs " << total << "\n";
    }

    void showInventory() {
        auto items = InventoryManager::getAllItems();
        std::cout << "\n\tCurrent Inventory Status\n";
        std::cout << "\n==================================================================";
        std::cout << "\n" << std::left << std::setw(20) << "Item" << std::setw(10) << "Price"
                  << std::setw(12) << "Available" << "Category";
        std::cout << "\n======================================================================";
        for (const auto& it : items)
            std::cout << "\n" << std::left << std::setw(20) << it.name << std::setw(10) << it.price
                      << std::setw(12) << it.quantity << it.category;
        std::cout << "\n=================================================================\n";
    }

    void archiveSales() {
        std::cout << "\nExport today's sales to CSV? (y/n): ";
        std::string c = readLine();
        if (c != "y" && c != "Y") return;

        std::time_t now = std::time(nullptr);
        char dateStr[16];
        std::strftime(dateStr, sizeof(dateStr), "%Y%m%d", std::localtime(&now));
        std::string filename = std::string("sales_report_") + dateStr + ".csv";

        if (ReportManager::exportSalesReport(filename))
            std::cout << "\nSales exported to " << filename << "\n";
        else
            std::cout << "\nError: could not create report file!\n";
    }

    void addNewUser() {
        std::cout << "\n=== Add New User ===\nUsername: ";
        std::string username = readLine();
        std::cout << "Password: ";
        std::string password = readLine();
        std::cout << "Role (admin/staff): ";
        std::string role = readLine();

        if (username.empty() || password.empty()) {
            std::cout << "\nUsername and password cannot be empty.";
            return;
        }
        if (role != "admin" && role != "staff") {
            std::cout << "\nInvalid role! Using 'staff'.";
            role = "staff";
        }
        std::cout << (UserManager::addUser(username, password, role)
                          ? "\nUser added successfully!"
                          : "\nFailed to add user. Username may already exist.");
    }
};

int main() {
    if (sodium_init() < 0) {
        std::cerr << "libsodium init failed\n";
        return 1;
    }
    HotelApp app;
    if (app.initialize()) app.run();
    return 0;
}
