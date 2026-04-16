import { initializeApp } from "https://www.gstatic.com/firebasejs/10.8.0/firebase-app.js";
import { getDatabase, ref, onValue } from "https://www.gstatic.com/firebasejs/10.8.0/firebase-database.js";

const firebaseConfig = {
    apiKey: "AIzaSyCgKeJBId5Ni2kR6hqma8Di08GPwoKtTBk",
    authDomain: "project-cow-database.firebaseapp.com",
    databaseURL: "https://project-cow-database-default-rtdb.firebaseio.com",
    projectId: "project-cow-database",
    storageBucket: "project-cow-database.firebasestorage.app",
    messagingSenderId: "885515230574",
    appId: "1:885515230574:web:771eecd27e44a68d07357f"
};

const app = initializeApp(firebaseConfig);
const db = getDatabase(app);

const cowSelect = document.getElementById("cowSelect");
const dateFilterInput = document.getElementById("dateFilter");
const startTimeInput = document.getElementById("startTimeFilter");
const endTimeInput = document.getElementById("endTimeFilter");
const viewModeSelect = document.getElementById("viewMode");
const graphFieldSelect = document.getElementById("graphField");
const metricContainer = document.getElementById("metricSelectContainer");
const applyBtn = document.getElementById("applyFilter");
const tableBody = document.getElementById("tableBody");
const userIdInput = document.getElementById("userIdInput");
const connectBtn = document.getElementById("connectBtn");
const setupFormContainer = document.getElementById('setupFormContainer');
const step1Instructions = document.getElementById('step1Instructions');

const singleGraphContainer = document.getElementById("singleGraphContainer");
const multiGraphGrid = document.getElementById("multiGraphGrid");

let cachedData = {};
let charts = new Array(6).fill(null);
let unsubscribeUser = null; // Holds the active Firebase listener cancellation function

const COLORS = {
    temperature: 'rgba(255, 99, 132, 1)',
    humidity: 'rgba(54, 162, 235, 1)',
    methane: 'rgba(255, 206, 86, 1)',
    co2: 'rgba(75, 192, 192, 1)',
    feedWeight: 'rgba(153, 102, 255, 1)',
    cattleWeight: 'rgba(255, 159, 64, 1)'
};

// --- EVENT LISTENERS ---
applyBtn.onclick = updateDashboard;
connectBtn.onclick = connectToUserDatabase;
cowSelect.onchange = updateDashboard;

viewModeSelect.onchange = () => {
    if (viewModeSelect.value === 'multi') {
        metricContainer.style.display = 'none';
    } else {
        metricContainer.style.display = 'block';
    }

    // Fade out current graphs
    singleGraphContainer.classList.remove("graph-visible");
    singleGraphContainer.classList.add("graph-fade");
    multiGraphGrid.classList.remove("graph-visible");
    multiGraphGrid.classList.add("graph-fade");

    // Wait for fade-out, then swap and fade in
    setTimeout(() => {
        updateDashboard();

        if (viewModeSelect.value === 'single') {
            singleGraphContainer.style.display = "block";
            multiGraphGrid.style.display = "none";
            setTimeout(() => singleGraphContainer.classList.add("graph-visible"), 50);
        } else {
            singleGraphContainer.style.display = "none";
            multiGraphGrid.style.display = "grid";
            setTimeout(() => multiGraphGrid.classList.add("graph-visible"), 50);
        }
    }, 300);
};


// 1. POPULATE COW SELECTOR
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

// 2. MAIN UPDATE FUNCTION
function updateDashboard() {
    const selectedCow = cowSelect.value;
    const selectedDate = dateFilterInput.value;
    const startTime = startTimeInput.value; // 24-hr start time string
    const endTime = endTimeInput.value;     // 24-hr end time string

    if (!selectedCow || !cachedData[selectedCow]) {
        tableBody.innerHTML = "<tr><td colspan='7'>No data found.</td></tr>";
        return;
    }

    const timestampsObj = cachedData[selectedCow];
    let sortedTimes = Object.keys(timestampsObj).sort((a, b) => b - a);

    let dates = [];
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

        const dateObj = new Date(ts * 1000);


        const year = dateObj.getFullYear();
        const month = String(dateObj.getMonth() + 1).padStart(2, '0');
        const day = String(dateObj.getDate()).padStart(2, '0');
        const dateString = `${year}-${month}-${day}`;
        dates.push(dateString);
        const timeString = dateObj.toLocaleTimeString();

        // 24-hr time string for filtering
        const hours24 = String(dateObj.getHours()).padStart(2, '0');
        const minutes = String(dateObj.getMinutes()).padStart(2, '0');
        const timeString24 = `${hours24}:${minutes}`;

        // FILTER 1: Date
        if (selectedDate && selectedDate !== dateString) return;

        // FILTER 2: Start Time (from app.js)
        if (startTime && timeString24 < startTime) return;

        // FILTER 3: End Time (from app.js)
        if (endTime && timeString24 > endTime) return;

        // Safe field access with ?? fallback (from app(1).js)
        const temperature = entry.temperature ?? "";
        const humidity = entry.humidity ?? "";
        const methane = entry.methane_ppm ?? "";
        const co2 = entry.co2 ?? "";
        //these lc#_kg are to do math with, 0-3 are the cow weight thing and 4-7 are the feed weight thing
        const lc0_kg = entry.lc0_kg ?? "";
        const lc1_kg = entry.lc1_kg ?? "";
        const lc2_kg = entry.lc2_kg ?? "";
        const lc3_kg = entry.lc3_kg ?? "";
        const lc4_kg = entry.lc4_kg ?? "";
        const lc5_kg = entry.lc5_kg ?? "";
        const lc6_kg = entry.lc6_kg ?? "";
        const lc7_kg = entry.lc7_kg ?? "";
        //now we do the math to get feed weight and cattle weight
        const feedWeight = Math.round(Math.abs(parseFloat(lc4_kg) + parseFloat(lc5_kg) + parseFloat(lc6_kg) + parseFloat(lc7_kg)) * 1000) / 1000;
        const cattleWeight = Math.round(Math.abs(parseFloat(lc0_kg) + parseFloat(lc1_kg) + parseFloat(lc2_kg) + parseFloat(lc3_kg)) * 1000) / 1000;

        //check for negatives????????


        // Table row includes feed/cattle weight columns (from app(1).js)
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

    tableBody.innerHTML = tableHtml || "<tr><td colspan='7'>No data for this date/time range.</td></tr>";

    //Reverse all arrays so time runs oldest → newest on the chart
    chartLabels.reverse();
    dataTemp.reverse();
    dataHum.reverse();
    dataMeth.reverse();
    dataCo2.reverse();
    dataFeedWeight.reverse();
    dataCattleWeight.reverse();
    dates.reverse();

    renderGraphs(chartLabels, dataTemp, dataHum, dataMeth, dataCo2, dataFeedWeight, dataCattleWeight, dates);
}

//Database Connection
function connectToUserDatabase() {
    let uid = userIdInput.value.trim();

    if (!uid) {
        alert("Please enter a valid User ID.");
        return;
    }

    // Cancel any existing listener before creating a new one
    if (unsubscribeUser) {
        unsubscribeUser();
    }

    const userRef = ref(db, "UsersData/" + uid);

    unsubscribeUser = onValue(userRef, (snapshot) => {
        const data = snapshot.val();
        cachedData = data || {};

        populateCowSelector();
        updateDashboard();
    });
}

// 4. RENDER GRAPHS (supports all 6 metrics)
function renderGraphs(labels, temp, hum, meth, co2, feedWeight, cattleWeight, dates) {
    // charts.forEach(c => c.destroy());
    // charts = [];

    const mode = viewModeSelect.value;

    if (mode === "single") {
        singleGraphContainer.style.display = "block";
        multiGraphGrid.style.display = "none";

        const field = graphFieldSelect.value;

        let dataToShow, colorToShow, labelToShow;

        if (field === "temperature") { dataToShow = temp; colorToShow = COLORS.temperature; labelToShow = "Temperature (°C)"; }
        else if (field === "humidity") { dataToShow = hum; colorToShow = COLORS.humidity; labelToShow = "Humidity (%)"; }
        else if (field === "methane") { dataToShow = meth; colorToShow = COLORS.methane; labelToShow = "Methane (ppm)"; }
        else if (field === "co2") { dataToShow = co2; colorToShow = COLORS.co2; labelToShow = "CO₂ (ppm)"; }
        else if (field === "feed weight") { dataToShow = feedWeight; colorToShow = COLORS.feedWeight; labelToShow = "Feed Weight (kg)"; }
        else if (field === "cattle weight") { dataToShow = cattleWeight; colorToShow = COLORS.cattleWeight; labelToShow = "Cattle Weight (kg)"; }

        const ctx = document.getElementById("mainChart").getContext("2d");
        if (charts[0] && charts[0].config.data.datasets[0].label === labelToShow) {
            charts[0].data.labels = labels;
            charts[0].data.datasets[0].data = dataToShow;
            charts[0].update('none'); //skips the animation
        } else {
            if (charts[0]) charts[0].destroy();
            charts[0] = new Chart(ctx, {
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
                    maintainAspectRatio: false,
                    plugins: {
                        tooltip: {
                            callbacks: {
                                title: function (tooltipItems) {
                                    const index = tooltipItems[0].dataIndex;
                                    return dates[index] + " at " + labels[index];
                                }
                            }
                        }
                    }
                }
            });
        }


    } else {
        singleGraphContainer.style.display = "none";
        multiGraphGrid.style.display = "grid";

        const multiConfigs = [
            { id: "chartTemp", label: "Temperature (°C)", data: temp, color: COLORS.temperature },
            { id: "chartHum", label: "Humidity (%)", data: hum, color: COLORS.humidity },
            { id: "chartMeth", label: "Methane (ppm)", data: meth, color: COLORS.methane },
            { id: "chartCo2", label: "CO₂ (ppm)", data: co2, color: COLORS.co2 },
            { id: "chartFeed", label: "Feed Weight (kg)", data: feedWeight, color: COLORS.feedWeight },
            { id: "chartCow", label: "Cattle Weight (kg)", data: cattleWeight, color: COLORS.cattleWeight },
        ];

        multiConfigs.forEach((cfg, i) => {
            if (charts[i]) {
                charts[i].data.labels = labels;
                charts[i].data.datasets[0].data = cfg.data;
                charts[i].update('none');
            } else {
                const ctx = document.getElementById(cfg.id).getContext("2d");
                charts[i] = new Chart(ctx, {
                    type: "line",
                    data: {
                        labels: labels,
                        datasets: [{
                            label: cfg.label,
                            data: cfg.data,
                            borderColor: cfg.color,
                            borderWidth: 2,
                            pointRadius: 0
                        }]
                    },
                    options: {
                        maintainAspectRatio: false,
                        plugins: { legend: { display: true } }
                    }
                });
            }
        });
    }
}

const urlParams = new URLSearchParams(window.location.search);
if (urlParams.has('uid')) {
    if (userIdInput) {
        userIdInput.value = urlParams.get('uid');
        connectToUserDatabase();
    }
} else {
    if (userIdInput) {
        userIdInput.value = "d7qH2bn6eLVEhkprSDApEc3RdFQ2";
        connectToUserDatabase();
    }
}