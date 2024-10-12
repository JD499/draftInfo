#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <SQLiteCpp/SQLiteCpp.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>


using json = nlohmann::json;

// Global vector to store player columns
std::vector<std::string> playerColumns;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string makeHttpRequest(const std::string& url) {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;
    curl = curl_easy_init();
    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if(res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
            return "";
        }
    }
    return readBuffer;
}

json safeJsonParse(const std::string& jsonString, const std::string& errorContext) {
    try {
        return json::parse(jsonString);
    } catch (const json::parse_error& e) {
        std::cerr << "JSON parse error in " << errorContext << ": " << e.what() << std::endl;
        std::cerr << "Response: " << jsonString.substr(0, 100) << "..." << std::endl;
        return json();
    }
}

void createPlayersTable(SQLite::Database& db, const std::vector<std::string>& columns) {
    std::string createTableSQL = "CREATE TABLE IF NOT EXISTS players (id TEXT PRIMARY KEY";
    for (const auto& column : columns) {
        if (column != "id") {
            createTableSQL += ", " + column + " TEXT";
        }
    }
    createTableSQL += ")";
    db.exec(createTableSQL);
}

void createLeagueTables(SQLite::Database& db) {
    db.exec("CREATE TABLE IF NOT EXISTS teams ("
            "id INTEGER PRIMARY KEY, "
            "name TEXT, "
            "owner_id TEXT)");

    db.exec("CREATE TABLE IF NOT EXISTS rosters ("
            "team_id INTEGER, "
            "player_id TEXT, "
            "FOREIGN KEY(team_id) REFERENCES teams(id), "
            "FOREIGN KEY(player_id) REFERENCES players(id), "
            "PRIMARY KEY(team_id, player_id))");
}

void fetchAndStorePlayersFromSleeper(SQLite::Database& db) {
    std::cout << "Fetching players from Sleeper API. This may take a moment..." << std::endl;

    std::string players_url = "https://api.sleeper.app/v1/players/nfl";
    std::string players_response = makeHttpRequest(players_url);
    json players_data = safeJsonParse(players_response, "players data");

    if (players_data.is_null()) {
        std::cerr << "Failed to fetch players data. Exiting." << std::endl;
        return;
    }

    // Collect all unique keys from the JSON objects
    std::set<std::string> column_set;
    for (const auto& [id, player] : players_data.items()) {
        column_set.insert("id");
        for (const auto& [key, value] : player.items()) {
            column_set.insert(key);
        }
    }

    // Convert set to vector for easier handling
    std::vector<std::string> columns(column_set.begin(), column_set.end());

    // Create the table with all columns
    createPlayersTable(db, columns);

    SQLite::Transaction transaction(db);

    try {
        int count = 0;

        // Construct the SQL INSERT statement
        std::stringstream ss;
        ss << "INSERT OR REPLACE INTO players (";
        for (size_t i = 0; i < columns.size(); ++i) {
            ss << columns[i];
            if (i < columns.size() - 1) ss << ", ";
        }
        ss << ") VALUES (";
        for (size_t i = 0; i < columns.size(); ++i) {
            ss << "?";
            if (i < columns.size() - 1) ss << ", ";
        }
        ss << ")";
        std::string insertSQL = ss.str();

        SQLite::Statement query(db, insertSQL);

        for (const auto& [id, player] : players_data.items()) {
            query.reset();
            int columnIndex = 1;
            for (const auto& column : columns) {
                if (column == "id") {
                    query.bind(columnIndex++, id);
                } else if (player.contains(column)) {
                    const auto& value = player[column];
                    if (value.is_string()) {
                        query.bind(columnIndex++, value.get<std::string>());
                    } else if (value.is_number_integer()) {
                        query.bind(columnIndex++, value.get<int64_t>());
                    } else if (value.is_number_float()) {
                        query.bind(columnIndex++, value.get<double>());
                    } else if (value.is_boolean()) {
                        query.bind(columnIndex++, value.get<bool>() ? 1 : 0);
                    } else if (value.is_null()) {
                        query.bind(columnIndex++);
                    } else {
                        query.bind(columnIndex++, value.dump());
                    }
                } else {
                    query.bind(columnIndex++);
                }
            }

            query.exec();

            count++;
            if (count % 100 == 0) {
                std::cout << "Processed " << count << " players..." << std::endl;
            }
        }

        transaction.commit();
        std::cout << "Successfully updated database with " << count << " players." << std::endl;
    }
    catch (std::exception& e) {
        std::cerr << "Error while updating database: " << e.what() << std::endl;
    }
}

void fetchAndStoreLeagueData(SQLite::Database& db, const std::string& league_id) {
    std::cout << "Fetching league data..." << std::endl;

    std::string users_url = "https://api.sleeper.app/v1/league/" + league_id + "/users";
    std::string users_response = makeHttpRequest(users_url);
    json users_data = safeJsonParse(users_response, "users data");

    if (users_data.is_null()) {
        std::cerr << "Failed to fetch users data. Exiting." << std::endl;
        return;
    }

    std::string rosters_url = "https://api.sleeper.app/v1/league/" + league_id + "/rosters";
    std::string rosters_response = makeHttpRequest(rosters_url);
    json rosters_data = safeJsonParse(rosters_response, "rosters data");

    if (rosters_data.is_null()) {
        std::cerr << "Failed to fetch rosters data. Exiting." << std::endl;
        return;
    }

    SQLite::Transaction transaction(db);

    try {
        SQLite::Statement insert_team(db, "INSERT OR REPLACE INTO teams (id, name, owner_id) VALUES (?, ?, ?)");
        for (const auto& user : users_data) {
            insert_team.bind(1, user["user_id"].get<std::string>());
            insert_team.bind(2, user["display_name"].get<std::string>());
            insert_team.bind(3, user["user_id"].get<std::string>());
            insert_team.exec();
            insert_team.reset();
        }

        SQLite::Statement insert_roster(db, "INSERT OR REPLACE INTO rosters (team_id, player_id) VALUES (?, ?)");
        for (const auto& roster : rosters_data) {
            std::string team_id = roster["owner_id"].get<std::string>();
            for (const auto& player_id : roster["players"]) {
                insert_roster.bind(1, team_id);
                insert_roster.bind(2, player_id.get<std::string>());
                insert_roster.exec();
                insert_roster.reset();
            }
        }

        transaction.commit();
        std::cout << "Successfully updated league data." << std::endl;
    }
    catch (std::exception& e) {
        std::cerr << "Error while updating league data: " << e.what() << std::endl;
    }
}

struct Player {
    std::string id;
    std::string full_name;
    std::string position;
    std::string nfl_team;
    std::string fantasy_team;
    int years_exp;
    int draft_year;
    int draft_round;
    int draft_pick;
    std::string draft_team;
};

std::vector<Player> players;

int getCurrentYear() {
    time_t now = time(0);
    tm *ltm = localtime(&now);
    return 1900 + ltm->tm_year;
}

void storePlayersInMemory(SQLite::Database& db) {
    players.clear();
    int current_year = getCurrentYear();

    // Query for rostered players
    std::string queryString = "SELECT p.id, p.full_name, p.position, t.name AS team_name, p.team AS nfl_team, p.years_exp "
                              "FROM players p "
                              "JOIN rosters r ON p.id = r.player_id "
                              "JOIN teams t ON r.team_id = t.id "
                              "WHERE p.team IS NOT NULL "
                              "AND p.position IN ('K', 'QB', 'RB', 'WR', 'TE')";
    if (std::find(playerColumns.begin(), playerColumns.end(), "status") != playerColumns.end()) {
        queryString += " AND p.status = 'Active'";
    }

    SQLite::Statement query(db, queryString);

    while (query.executeStep()) {
        Player player;
        player.id = query.getColumn(0).getText();
        player.full_name = query.getColumn(1).getText();
        player.position = query.getColumn(2).getText();
        player.fantasy_team = query.getColumn(3).getText();
        player.nfl_team = query.getColumn(4).getText();
        player.years_exp = query.getColumn(5).getInt();
        player.draft_year = current_year - player.years_exp;
        players.push_back(player);
    }

    // Query for unrostered players
    std::string query2String = "SELECT id, full_name, position, team, years_exp "
                               "FROM players "
                               "WHERE id NOT IN (SELECT player_id FROM rosters) "
                               "AND team IS NOT NULL "
                               "AND position IN ('K', 'QB', 'RB', 'WR', 'TE')";
    if (std::find(playerColumns.begin(), playerColumns.end(), "status") != playerColumns.end()) {
        query2String += " AND status = 'Active'";
    }

    SQLite::Statement query2(db, query2String);

    while (query2.executeStep()) {
        Player player;
        player.id = query2.getColumn(0).getText();
        player.full_name = query2.getColumn(1).getText();
        player.position = query2.getColumn(2).getText();
        player.nfl_team = query2.getColumn(3).getText();
        player.fantasy_team = "Unrostered";
        player.years_exp = query2.getColumn(4).getInt();
        player.draft_year = current_year - player.years_exp;
        players.push_back(player);
    }
}

std::string cleanName(const std::string& name) {
    std::string cleaned = name;
    // Remove non-breaking spaces (UTF-8 encoded)
    cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '\xC2'), cleaned.end());
    cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '\xA0'), cleaned.end());
    // Remove trailing spaces
    while (!cleaned.empty() && std::isspace(cleaned.back())) {
        cleaned.pop_back();
    }
    // Convert to lowercase for case-insensitive comparison
    std::transform(cleaned.begin(), cleaned.end(), cleaned.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return cleaned;
}

std::pair<int, int> getDraftYearRange(const std::vector<Player>& players) {
    int currentYear = getCurrentYear();
    int oldestDraftYear = currentYear;

    for (const auto& player : players) {
        if (player.years_exp > 0) {
            int potentialDraftYear = currentYear - player.years_exp;
            if (potentialDraftYear < oldestDraftYear) {
                oldestDraftYear = potentialDraftYear;
            }
        }
    }

    return std::make_pair(oldestDraftYear, currentYear);
}

void fetchDraftInformation(int year) {
    std::string url = "https://en.wikipedia.org/wiki/" + std::to_string(year) + "_NFL_draft";
    std::string html = makeHttpRequest(url);

    std::cout << "Fetching draft information for " << year << std::endl;
    std::cout << "Fetched HTML content. Length: " << html.length() << " characters." << std::endl;

    htmlDocPtr doc = htmlReadMemory(html.c_str(), html.length(), url.c_str(), NULL, HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    if (doc == NULL) {
        std::cerr << "Failed to parse HTML for " << year << std::endl;
        return;
    }
    std::cout << "HTML parsed successfully for " << year << std::endl;

    xmlXPathContextPtr context = xmlXPathNewContext(doc);
    if (context == NULL) {
        std::cerr << "Failed to create XPath context" << std::endl;
        xmlFreeDoc(doc);
        return;
    }
    std::cout << "XPath context created." << std::endl;

    // Find the specific table with all three classes
    xmlXPathObjectPtr result = xmlXPathEvalExpression(BAD_CAST "//table[contains(@class, 'wikitable') and contains(@class, 'sortable') and contains(@class, 'plainrowheaders')]", context);
    if (result == NULL) {
        std::cerr << "XPath query failed" << std::endl;
        xmlXPathFreeContext(context);
        xmlFreeDoc(doc);
        return;
    }

    xmlNodeSetPtr tables = result->nodesetval;
    std::cout << "Found " << tables->nodeNr << " tables with the specific classes" << std::endl;

    if (tables->nodeNr == 0) {
        std::cout << "Could not find the draft table in the HTML." << std::endl;
        xmlXPathFreeObject(result);
        xmlXPathFreeContext(context);
        xmlFreeDoc(doc);
        return;
    }

    xmlNodePtr table = tables->nodeTab[0];  // We expect only one table with these specific classes
    std::cout << "Processing the draft table..." << std::endl;

    // Find the tbody element
    xmlNodePtr tbody = table->children;
    while (tbody && xmlStrcmp(tbody->name, (const xmlChar *)"tbody") != 0) {
        tbody = tbody->next;
    }

    if (tbody) {
        // Process each row in the tbody
        xmlNodePtr row = tbody->children;
        int rowCount = 0;
        while (row) {
            if (row->type == XML_ELEMENT_NODE && xmlStrcmp(row->name, (const xmlChar *)"tr") == 0) {
                rowCount++;
                std::vector<std::string> rowData;
                xmlNodePtr cell = row->children;
                while (cell) {
                    if (cell->type == XML_ELEMENT_NODE &&
                       (xmlStrcmp(cell->name, (const xmlChar *)"td") == 0 ||
                        xmlStrcmp(cell->name, (const xmlChar *)"th") == 0)) {
                        xmlChar* content = xmlNodeGetContent(cell);
                        if (content) {
                            rowData.push_back((char*)content);
                            xmlFree(content);
                        } else {
                            rowData.push_back("");
                        }
                    }
                    cell = cell->next;
                }

                std::cout << "Row " << rowCount << " data:" << std::endl;
                for (size_t i = 0; i < rowData.size(); ++i) {
                    std::cout << "  Column " << i << ": " << rowData[i] << std::endl;
                }

                if (rowData.size() >= 5) {
                    try {
                        int round = std::stoi(rowData[1]);
                        int pick = std::stoi(rowData[2]);
                        std::string team = rowData[3];
                        std::string player_name = cleanName(rowData[4]);

                        std::cout << "Processed: Round " << round << ", Pick " << pick
                                  << ", Team " << team << ", Player " << player_name << std::endl;

                        // Update player information
                        bool playerFound = false;
                        for (auto& player : players) {
                            if (cleanName(player.full_name) == player_name) {
                                player.draft_round = round;
                                player.draft_pick = pick;
                                player.draft_team = team;
                                playerFound = true;
                                std::cout << "Updated player information for " << player_name << std::endl;
                                break;
                            }
                        }
                        if (!playerFound) {
                            std::cout << "Player not found in database: " << player_name << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Error processing row: " << e.what() << std::endl;
                    }
                } else {
                    std::cout << "Row " << rowCount << " does not have enough columns." << std::endl;
                }
            }
            row = row->next;
        }
        std::cout << "Processed " << rowCount << " rows from the draft table." << std::endl;
    } else {
        std::cout << "Could not find tbody in the draft table." << std::endl;
    }

    xmlXPathFreeObject(result);
    xmlXPathFreeContext(context);
    xmlFreeDoc(doc);
    std::cout << "Finished processing draft information." << std::endl;
}


void displayPlayers() {
    std::cout << "\nPlayers (K, QB, RB, WR, TE) on NFL Teams:\n";
    for (const auto& player : players) {
        std::cout << player.full_name << " (" << player.position << ") - "
                  << "NFL Team: " << player.nfl_team << ", "
                  << "Fantasy Team: " << player.fantasy_team << ", "
                  << "Draft Year: " << player.draft_year << std::endl;
    }
}

void displayPlayersWithDraftInfo() {
    std::cout << "\nPlayers with Draft Information:\n";
    for (const auto& player : players) {
        if (player.draft_round > 0) {
            std::cout << player.full_name << " (" << player.position << ") - "
                      << "NFL Team: " << player.nfl_team << ", "
                      << "Fantasy Team: " << player.fantasy_team << ", "
                      << "Draft: Year " << player.draft_year << ", Round " << player.draft_round << ", Pick " << player.draft_pick << " by " << player.draft_team << std::endl;
        }
    }
}

/*
 * IT MESSES UP WITH THE YELLOW PLAYERS THAT HAVE THE CROSS (PRO BOWLERS) ALSO ANY COLORED ONE. ALSO ITS ONLY DOING PLAYERS ON ROSTERS RN Also does not do brian thomas bc name difference. marvin harris jr
*<td bgcolor="#faecc8"><span data-sort-value="Kupp, Cooper"><span class="vcard"><span class="fn"><a href="/wiki/Cooper_Kupp" title="Cooper Kupp">Cooper Kupp</a></span></span></span>&nbsp;<sup>†</sup>
</td>
*/

int main() {
    try {
        SQLite::Database db("fantasy_league.db", SQLite::OPEN_CREATE | SQLite::OPEN_READWRITE);

        curl_global_init(CURL_GLOBAL_DEFAULT);

        std::string league_id;
        std::cout << "Enter your Sleeper league ID: ";
        std::cin >> league_id;

        std::cout << "Do you want to update the player database from Sleeper? (Y/N): ";
        std::string response;
        std::cin >> response;

        if (response == "Y" || response == "y") {
            fetchAndStorePlayersFromSleeper(db);
        } else {
            std::cout << "Skipping player database update." << std::endl;
            SQLite::Statement query(db, "PRAGMA table_info(players)");
            while (query.executeStep()) {
                playerColumns.push_back(query.getColumn(1).getText());
            }
        }

        createLeagueTables(db);
        fetchAndStoreLeagueData(db, league_id);

        storePlayersInMemory(db);

        // Get the range of years to fetch draft information
        auto [startYear, endYear] = getDraftYearRange(players);
        std::cout << "Fetching draft information for years " << startYear << " to " << endYear << std::endl;

        // Fetch draft information for each year
        for (int year = startYear; year <= endYear; ++year) {
            fetchDraftInformation(year);
        }

        displayPlayersWithDraftInfo();

        curl_global_cleanup();
    }
    catch (std::exception& e) {
        std::cout << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}