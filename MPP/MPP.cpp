#include <condition_variable>
#include <stdexcept>
#include <iostream>
#include <csignal>
#include <climits>
#include <iomanip>
#include <vector>
#include <math.h>
#include <future>
#include <chrono>
#include <random>
#include <thread>
#include <mutex>

using namespace std;

atomic<bool> stopSignal = false;

mutex updateMutex;
mutex accessMutex;
mutex failCountMutex;

condition_variable refreshCondition;

atomic<bool> needAnUpdate    = false;
atomic<int> failedMatches    = 0;
atomic<int> completedDeals   = 0;
atomic<int> blockedCashboxes = 0;
atomic<int> countCashboxes   = 0;

struct Client {
    string operation;
    int value;
    bool isReserved;
    bool isDone;
};

struct Time {
    chrono::duration<double> workingTime = chrono::seconds(0);
    chrono::duration<double> waitingTime = chrono::seconds(0);
};

void abortExecution(int signalCode) {
    cout << "\t<force termination>\n";
    stopSignal.store(true);
    refreshCondition.notify_all();
}

// обновляет вышедших юзеров (работает по кд)
void queueCleaner(vector<Client*>& queue) {
    while (!stopSignal) {
        {
            random_device rd;
            mt19937 gen(rd());
            uniform_int_distribution<int> valuesIndex(0, 2);
            uniform_int_distribution<int> zeroOrOne(0, 1);
            vector<int> values = { 2, 6, 12 };

            for (Client*& client : queue) {
                if (client->isDone) {
                    lock_guard<mutex> lock(accessMutex);
                    client->value = values[valuesIndex(gen)];
                    client->operation = zeroOrOne(gen) ? "buy" : "sell";
                    client->isReserved = false;
                    client->isDone = false;

                    cout << "New client: " << client->operation << " - " << client->value << "\n";
                    break;
                }
            }
        }
        this_thread::sleep_for(chrono::milliseconds(1000));
    }
}

// полное обновление очереди. аварийный случай
void hotClean(vector<Client*>& queue) {
    while (!stopSignal) {
        unique_lock<mutex> lock(updateMutex);
        refreshCondition.wait(lock, [] { return needAnUpdate || stopSignal; });

        if (stopSignal) break;

        random_device rd;
        mt19937 gen(rd());
        uniform_int_distribution<int> valuesIndex(0, 2);
        uniform_int_distribution<int> zeroOrOne(0, 1);
        vector<int> values = { 2, 6, 12 };

        {
            lock_guard<mutex> queueLock(accessMutex);
            for (Client*& client : queue) {
                if (!client->isReserved) {
                    client->value = values[valuesIndex(gen)];
                    client->operation = zeroOrOne(gen) ? "buy" : "sell";
                    client->isReserved = false;
                    client->isDone = false;
                }
            }
        }

        cout << "\t<Queue updated>\n";
        this_thread::sleep_for(chrono::milliseconds(1000));
        needAnUpdate = false;
        refreshCondition.notify_all();
    }
}

bool matchPartner(vector<Client*>& queue, Client*& client) {
    int neededSum = client->value;
    string expectedOperation = client->operation;
    vector<Client*> foundedPartners;
    int accumulated = 0;

    //lock_guard<mutex> lock(accessMutex);
    for (Client*& candidate : queue) {
        if (!candidate->isReserved
            && !candidate->isDone
            && candidate->operation == expectedOperation
            && candidate != client) {
            if (accumulated + candidate->value <= neededSum) {
                candidate->isReserved = true;
                foundedPartners.push_back(candidate);
                accumulated += candidate->value;
            }

            if (accumulated == neededSum) {
                lock_guard<mutex> lock(accessMutex);
                for (Client*& partner : foundedPartners) {
                    partner->isDone = true;
                }
                this_thread::sleep_for(chrono::milliseconds(500));
                completedDeals++;
                cout << "The deal number " << completedDeals << " is completed. Thread ID: " << this_thread::get_id() << "\n";
                return true;
            }
        }
    }
    for (Client*& client : foundedPartners)
        client->isReserved = false;
    return false;
}

// дефолт кассовое обслуживание
void processing(vector<Client*>& queue, Time& metrics) {
    chrono::steady_clock::time_point start = chrono::steady_clock::now();

    while (!stopSignal) {
        Client* current = nullptr;
        {
            lock_guard<mutex> lock(accessMutex);
            for (Client*& client : queue) {
                if (!client->isReserved && !client->isDone) {
                    client->isReserved = true;
                    current = client;
                    break;
                }
            }
        }
        bool alreadyBlocked = false;
        if (current == nullptr) continue;
        while (!current->isDone && !stopSignal) {
            if (!matchPartner(queue, current)) {
                {
                    lock_guard<mutex> failLock(failCountMutex);
                    blockedCashboxes++;
                }
                needAnUpdate = false;
                {
                    lock_guard<mutex> failLock(failCountMutex);
                    if (blockedCashboxes >= countCashboxes) {
                        blockedCashboxes.store(0);

                        needAnUpdate = true;
                        refreshCondition.notify_all();
                    }
                }

                chrono::steady_clock::time_point waitBegin = chrono::steady_clock::now();
                unique_lock<mutex> lock(updateMutex);

                refreshCondition.wait(lock, [] { return !needAnUpdate || stopSignal; });

                chrono::steady_clock::time_point waitEnd = chrono::steady_clock::now();
                chrono::milliseconds waitResult = chrono::duration_cast<chrono::milliseconds>(waitEnd - waitBegin);
                metrics.waitingTime += waitResult;
            }
            else {
                lock_guard<mutex> lock(accessMutex);
                current->isDone = true;
                break;
            }
        }
    }

    chrono::steady_clock::time_point end = chrono::steady_clock::now();
    metrics.workingTime = end - start - metrics.waitingTime;
}

int main() {
    signal(SIGINT, abortExecution);

    int cashboxesCount, queueLength;
    cout << "Number of cashboxes: ";
    cin >> cashboxesCount;
    cout << "Queue length: ";
    cin >> queueLength;
    countCashboxes = cashboxesCount;

    vector<Client*> clientQueue(queueLength);
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> valuesIndex(0, 2);
    uniform_int_distribution<int> zeroOrOne(0, 1);
    vector<int> values = { 2, 6, 12 };

    for (int i = 0; i < queueLength; ++i) {
        clientQueue[i] = new Client();
        clientQueue[i]->value = values[valuesIndex(gen)];
        clientQueue[i]->operation = zeroOrOne(gen) ? "buy" : "sell";
    }

    vector<thread> cashboxesThreads;
    vector<Time> allStats(cashboxesCount);

    for (int i = 0; i < cashboxesCount; ++i)
        cashboxesThreads.emplace_back(processing, ref(clientQueue), ref(allStats[i]));

    thread maintenanceThread(hotClean, ref(clientQueue));
    thread generatorThread(queueCleaner, ref(clientQueue));


    for (thread& thread : cashboxesThreads)
        thread.join();


    maintenanceThread.join();
    generatorThread.join();

    cout << "\nTotal duration:\n";
    for (int i = 0; i < cashboxesCount; ++i) {
        cout << "Cashbox n:" << i + 1 << ": \n";
        cout << "\tWorking time: " << allStats[i].workingTime.count() << " s.\n";
        cout << "\tWaiting time: " << allStats[i].waitingTime.count() << " s.\n";
    }

    for (Client* ones : clientQueue) delete ones;
    return 0;
}


//Используя future/promise, напишите программу для поиска индекса наименьшего и наибольшего элемента в массиве при
// помощи асинхронных вызовов функции (далее, задач) поиска на интервале внутри массива (поиск на одном интервал
// равен одной задаче). Массив состоит из случайных целочисленных значений (количество элементов в массиве более
// 100 000, значение одного элемента в интервале от -2147483648 до 2147483647). Количество элементов задаётся
// пользователем. Для одного массива измерьте время выполнения поиска для 2, 4, 8, 16 задач. Для каждого количества
// задач выведите затраченное на выполнение задачи время.

//Напишите программу, которая моделирует систему массового обслуживания с накопителем конечной ёмкости.
// Приборы разделены на n (n > 2) групп, одна группа приборов обслуживает один класс заявок. Внутри класса
// заявки разделены на три типа: заявки третьего типа имеют наивысший приоритет, заявки первого типа - наименьший.
// В каждой группе m (m > 2) приборов. Все типы заявок находятся в общей очереди. Работа одного прибора происходит в
// отдельном потоке. После извлечения заявки из очереди, поток прибора засыпает на случайное время. После окончания
// этого времени, прибор считается свободным. Ёмкость накопителя, количество групп приборов, количество приборов в
// каждой группе задаётся пользователем. Генератор через случайные промежутки времени добавляет в очередь заявки
// из разных групп со случайным типов. Генератор неактивен, если в очереди нет свободных мест. Генератор работает
// в отдельном потоке. Во время работы программы на экран выводится информация о состоянии приборов (занят (каким
// типом и сколько времени осталось до завершения обработки), свободен) и количестве элементов в очереди.

// Напишите программу, которая моделирует банки n-ой страны.
// В начале пользователь задаёт 2 параметра(Размерность общей очереди, количество касс, которые обрабатывают пользователей)
// Очередь является общим ресурсом для всех касс, генерация людей в очередь происходит в отдельном потоке.
// Люди бывают двух видов, те кто продают и те кто покупают.Сумма денег которую хотят купить / продать может быть равна одному из трёх
// значений 2, 6, 12
// Каждая касса работает в отдельном потоке и обрабатывает людей.Обработка начинается с того, что на кассу приходит первый человек из
// очереди, после того как человек подошёл к кассе, касса обрабатывает его заявку и ищет ему подходящего партнера / партнеров, которые есть в очереди
// Пример : на кассу подошел человек с заявкой на покупку 12 монет.Касса должна найти человека в очереди, который хочет продать 12
// монет, либо несколько людей которые хотят продать свои монеты и сумма их монет равняется 12. В момент когда касса начинает искать людей в
// очереди, она резервирует конкретного человека и другие кассы не могут его запросить для обработки своей заявки
// Если ни одна из касс не может найти нужного партнёра / партнёров из очереди и очередь заполнена, то кассы требует очистить очередь.
// Запускается отдельный поток, который очищает очередь(кроме зарезервированных людей).
// При остановки программы, в консоле должно вывестись время в течении которого кассы не могли обрабатывать людей и время для каждой
// кассы в течении которого они обрабатывали заявки.
