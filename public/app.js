// --- IMPORTS ---
// Importing Firebase tools to connect to your database
import { initializeApp } from "https://www.gstatic.com/firebasejs/10.8.0/firebase-app.js";
import { getDatabase, ref, onValue } from "https://www.gstatic.com/firebasejs/10.8.0/firebase-database.js";

// --- CONFIGURATION ---
// Your specific Firebase project keys
const firebaseConfig = {
  apiKey: "AIzaSyCgKeJBId5Ni2kR6hqma8Di08GPwoKtTBk",
  authDomain: "project-cow-database.firebaseapp.com",
  databaseURL: "https://project-cow-database-default-rtdb.firebaseio.com",
  projectId: "project-cow-database",
  storageBucket: "project-cow-database.firebasestorage.app",
  messagingSenderId: "885515230574",
  appId: "1:885515230574:web:771eecd27e44a68d07357f"
};

// Initialize the app and database connection
const app = initializeApp(firebaseConfig);
const db = getDatabase(app);

// Hardcoded User ID (The owner of the cows)
const userId = "d7qH2bn6eLVEhkprSDApEc3RdFQ2";

// --- DOM ELEMENTS ---
const cowSelect = document.getElementById("cowSelect");
const dateFilterInput = document.getElementById("dateFilter");
const viewModeSelect = document.getElementById("viewMode");
const graphFieldSelect = document.getElementById("graphField");
const metricContainer = document.getElementById("metricSelectContainer");
const applyBtn = document.getElementById("applyFilter");
const tableBody = document.getElementById("tableBody");

// Graph Containers
const singleGraphContainer = document.getElementById("singleGraphContainer");
const multiGraphGrid = document.getElementById("multiGraphGrid");

// --- STATE VARIABLES ---
let cachedData = {};
let charts = [];

// --- COLOR PALETTE ---
const COLORS = {
    temperature: 'rgba(255, 99, 132, 1)',
    humidity: 'rgba(54, 162, 235, 1)',
    methane: 'rgba(255, 206, 86, 1)',
    co2: 'rgba(75, 192, 192, 1)',
    feedWeight: 'rgba(153, 102, 255, 1)',
    cattleWeight: 'rgba(255, 159, 64, 1)'
};

// --- REALTIME LISTENER ---
const userRef = ref(db, "UsersData/" + userId);

onValue(userRef, (snapshot) => {
    const data = snapshot.val();
    cachedData = data || {};
    
    populateCowSelector();
    updateDashboard();
});

// --- EVENT LISTENERS ---
applyBtn.onclick = updateDashboard;

viewModeSelect.onchange = () => {
    if (viewModeSelect.value === 'multi') {
        metricContainer.style.display = 'none';
    } else {
        metricContainer.style.display = 'block';
    }

    singleGraphContainer.classList.remove("graph-visible");
    singleGraphContainer.classList.add("graph-fade");
    
    multiGraphGrid.classList.remove("graph-visible");
    multiGraphGrid.classList.add("graph-fade");

    setTimeout(() => {
        updateDashboard();
        
        if (viewModeSelect.value === 'single') {
            singleGraphContainer.style.display = "block";
            multiGraphGrid.style.display = "none";
            setTimeout(() => {
                singleGraphContainer.classList.add("graph-visible");
            }, 50);
        } else {
            singleGraphContainer.style.display = "none";
            multiGraphGrid.style.display = "grid";
            setTimeout(() => {
                multiGraphGrid.classList.add("graph-visible");
            }, 50);
        }
    }, 300);
};

// --- FUNCTIONS ---

function populateCowSelector() {
    const currentSelection = cowSelect.value;
    const rfids = Object.keys(cachedData);
    
    cowSelect.innerHTML = "";

    rfids.forEach(rfid => {
        const option = document.createElement("option");
        option.value = rfid;
        option.innerText = `Cow ${rfid}`;
        cowSelect.appendChild(option);
    });

    if (rfids.includes(currentSelection)) {
        cowSelect.value = currentSelection;
    } else if (rfids.length > 0) {
        cowSelect.value = rfids[0];
    }
}

function updateDashboard() {
    const selectedCow = cowSelect.value;
    const selectedDate = dateFilterInput.value;
    
    if (!selectedCow || !cachedData[selectedCow]) {
        tableBody.innerHTML = "<tr><td colspan='7'>No data found.</td></tr>";
        return;
    }

    const timestampsObj = cachedData[selectedCow];
    let sortedTimes = Object.keys(timestampsObj).sort((a, b) => b - a);

    let chartLabels = [];
    let dataTemp = [];
    let dataHum = [];
    let dataMeth = [];
    let dataCo2 = [];
    let dataFeedWeight = [];
    let dataCattleWeight = [];

    let tableHtml = "";

    sortedTimes.forEach(ts => {
        const entry = timestampsObj[ts];
        
        let dateObj = new Date(ts * 1000);
        let dateString = dateObj.toISOString().split("T")[0];
        let timeString = dateObj.toLocaleTimeString();

        if (selectedDate && selectedDate !== dateString) return;

        const temperature = entry.temperature ?? "";
        const humidity = entry.humidity ?? "";
        const methane = entry.methane ?? "";
        const co2 = entry.co2 ?? "";
        const feedWeight = entry["feed weight"] ?? entry.feedWeight ?? "";
        const cattleWeight = entry["cattle weight"] ?? entry.cattleWeight ?? "";

        tableHtml += `
            <tr>
                <td>${dateString} ${timeString}</td>
                <td>${temperature}</td>
                <td>${humidity}</td>
                <td>${methane}</td>
                <td>${co2}</td>
                <td>${feedWeight}</td>
                <td>${cattleWeight}</td>
            </tr>
        `;

        chartLabels.push(timeString);
        dataTemp.push(temperature);
        dataHum.push(humidity);
        dataMeth.push(methane);
        dataCo2.push(co2);
        dataFeedWeight.push(feedWeight);
        dataCattleWeight.push(cattleWeight);
    });

    tableBody.innerHTML = tableHtml || "<tr><td colspan='7'>No data for this date.</td></tr>";

    chartLabels.reverse();
    dataTemp.reverse();
    dataHum.reverse();
    dataMeth.reverse();
    dataCo2.reverse();
    dataFeedWeight.reverse();
    dataCattleWeight.reverse();

    renderGraphs(
        chartLabels,
        dataTemp,
        dataHum,
        dataMeth,
        dataCo2,
        dataFeedWeight,
        dataCattleWeight
    );
}

function renderGraphs(labels, temp, hum, meth, co2, feedWeight, cattleWeight) {
    charts.forEach(c => c.destroy());
    charts = [];

    const mode = viewModeSelect.value;

    if (mode === "single") {
        singleGraphContainer.style.display = "block";
        multiGraphGrid.style.display = "none";

        const field = graphFieldSelect.value;
        
        let dataToShow, colorToShow, labelToShow;

        if (field === "temperature") {
            dataToShow = temp;
            colorToShow = COLORS.temperature;
            labelToShow = "Temperature";
        } else if (field === "humidity") {
            dataToShow = hum;
            colorToShow = COLORS.humidity;
            labelToShow = "Humidity";
        } else if (field === "methane") {
            dataToShow = meth;
            colorToShow = COLORS.methane;
            labelToShow = "Methane";
        } else if (field === "co2") {
            dataToShow = co2;
            colorToShow = COLORS.co2;
            labelToShow = "CO₂";
        } else if (field === "feed weight") {
            dataToShow = feedWeight;
            colorToShow = COLORS.feedWeight;
            labelToShow = "Feed Weight";
        } else if (field === "cattle weight") {
            dataToShow = cattleWeight;
            colorToShow = COLORS.cattleWeight;
            labelToShow = "Cattle Weight";
        }

        const ctx = document.getElementById("mainChart").getContext("2d");
        const newChart = new Chart(ctx, {
            type: "line",
            data: {
                labels: labels,
                datasets: [{
                    label: labelToShow,
                    data: dataToShow,
                    borderColor: colorToShow,
                    backgroundColor: colorToShow.replace('1)', '0.1)'),
                    borderWidth: 2,
                    fill: true,
                    tension: 0.3
                }]
            },
            options: {
                maintainAspectRatio: false
            }
        });

        charts.push(newChart);

    } else {
        singleGraphContainer.style.display = "none";
        multiGraphGrid.style.display = "grid";

        const createSmallChart = (id, label, data, color) => {
            const ctx = document.getElementById(id).getContext("2d");
            return new Chart(ctx, {
                type: "line",
                data: {
                    labels: labels,
                    datasets: [{
                        label: label,
                        data: data,
                        borderColor: color,
                        borderWidth: 2,
                        pointRadius: 0
                    }]
                },
                options: { 
                    maintainAspectRatio: false,
                    plugins: { legend: { display: true } }
                }
            });
        };

        charts.push(createSmallChart("chartTemp", "Temperature", temp, COLORS.temperature));
        charts.push(createSmallChart("chartHum", "Humidity", hum, COLORS.humidity));
        charts.push(createSmallChart("chartMeth", "Methane", meth, COLORS.methane));
        charts.push(createSmallChart("chartCo2", "CO₂", co2, COLORS.co2));
        charts.push(createSmallChart("chartCow", "Cattle Weight", cattleWeight, COLORS.cattleWeight));
        charts.push(createSmallChart("chartFeed", "Feed Weight", feedWeight, COLORS.feedWeight));
    }
}
