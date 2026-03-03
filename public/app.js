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
// Grab references to HTML elements so we can manipulate them
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
let cachedData = {};  // Stores all raw data from Firebase
let charts = [];      // Array to store Chart instances so we can destroy them before redrawing

// --- COLOR PALETTE ---
// Define specific colors for specific data types for consistency
const COLORS = {
    temperature: 'rgba(255, 99, 132, 1)',   // Red
    humidity: 'rgba(54, 162, 235, 1)',      // Blue
    methane: 'rgba(255, 206, 86, 1)',       // Yellow/Orange
    co2: 'rgba(75, 192, 192, 1)'            // Green
};

// --- REALTIME LISTENER ---
// Connects to "UsersData/userID" and listens for ANY change.
const userRef = ref(db, "UsersData/" + userId);

onValue(userRef, (snapshot) => {
    const data = snapshot.val();
    cachedData = data || {};
    
    // Once we have data, we populate the "Select Cow" dropdown
    populateCowSelector();
    
    // Automatically trigger the update to show data immediately
    updateDashboard();
});

// --- EVENT LISTENERS ---
// When the "Update View" button is clicked
applyBtn.onclick = updateDashboard;

// When "View Mode" changes, toggle the visibility of the "Metric" dropdown
viewModeSelect.onchange = () => {
    if(viewModeSelect.value === 'multi') {
        metricContainer.style.display = 'none'; // Hide metric select for multi view
    } else {
        metricContainer.style.display = 'block'; // Show it for single view
    }
    updateDashboard();

    // 2. ANIMATE GRAPHS (Cross-fade Effect)
    // First, fade out the current content
    singleGraphContainer.classList.remove("graph-visible");
    singleGraphContainer.classList.add("graph-fade");
    
    multiGraphGrid.classList.remove("graph-visible");
    multiGraphGrid.classList.add("graph-fade");

    // Wait 300ms for fade-out to finish, then swap data and fade in
    setTimeout(() => {
        updateDashboard(); // Render the new graphs while invisible
        
        // Trigger Fade In
        if (viewModeSelect.value === 'single') {
            singleGraphContainer.style.display = "block";
            multiGraphGrid.style.display = "none";
            
            // We use a tiny timeout to allow the browser to process the 'display: block' 
            // before applying the opacity change, otherwise it won't animate.
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

// 1. POPULATE COW SELECTOR
// Looks at the keys in cachedData (which are RFIDs) and fills the dropdown
function populateCowSelector() {
    // Save the currently selected cow (if any) so we don't lose selection on refresh
    const currentSelection = cowSelect.value;
    
    // Get all keys (RFIDs) from the data object
    const rfids = Object.keys(cachedData);
    
    // Reset dropdown HTML
    cowSelect.innerHTML = "";

    rfids.forEach(rfid => {
        const option = document.createElement("option");
        option.value = rfid;
        option.innerText = `Cow ${rfid}`; // Label it nicely
        cowSelect.appendChild(option);
    });

    // Restore selection if it still exists, otherwise select the first one
    if (rfids.includes(currentSelection)) {
        cowSelect.value = currentSelection;
    } else if (rfids.length > 0) {
        cowSelect.value = rfids[0];
    }
}

// 2. MAIN UPDATE FUNCTION
// Orchestrates the filtering, table rendering, and graph rendering
function updateDashboard() {
    const selectedCow = cowSelect.value;
    const selectedDate = dateFilterInput.value;
    
    // Safety check: if no data or no cow selected, stop.
    if (!selectedCow || !cachedData[selectedCow]) {
        tableBody.innerHTML = "<tr><td colspan='5'>No data found.</td></tr>";
        return;
    }

    // Get the timestamps for the selected cow
    const timestampsObj = cachedData[selectedCow];
    
    // Sort timestamps (Newest first) so the table shows latest data at top
    // Note: Timestamps in Firebase keys are usually strings, we subtract to sort numerically
    let sortedTimes = Object.keys(timestampsObj).sort((a, b) => b - a);

    // Arrays to hold data for the charts
    // We reverse these later because charts look better reading Oldest -> Newest (Left to Right)
    let chartLabels = [];
    let dataTemp = [];
    let dataHum = [];
    let dataMeth = [];
    let dataCo2 = [];

    // Clear Table
    let tableHtml = "";

    sortedTimes.forEach(ts => {
        const entry = timestampsObj[ts];
        
        // Convert Epoch time to readable Date
        // (Assuming ts is seconds. If it's milliseconds, remove the *1000)
        let dateObj = new Date(ts * 1000);
        let dateString = dateObj.toISOString().split("T")[0]; // YYYY-MM-DD
        let timeString = dateObj.toLocaleTimeString();

        // FILTER: If a date is selected and this entry doesn't match, skip it
        if (selectedDate && selectedDate !== dateString) return;

        // Add row to Table HTML
        tableHtml += `
            <tr>
                <td>${dateString} ${timeString}</td>
                <td>${entry.temperature}</td>
                <td>${entry.humidity}</td>
                <td>${entry.methane}</td>
                <td>${entry.co2}</td>
            </tr>
        `;

        // Add data to Chart Arrays
        chartLabels.push(timeString);
        dataTemp.push(entry.temperature);
        dataHum.push(entry.humidity);
        dataMeth.push(entry.methane);
        dataCo2.push(entry.co2);
    });

    // Update the DOM with the new table rows
    tableBody.innerHTML = tableHtml || "<tr><td colspan='5'>No data for this date.</td></tr>";

    // Reverse arrays for the chart (so time goes Left->Right)
    chartLabels.reverse();
    dataTemp.reverse();
    dataHum.reverse();
    dataMeth.reverse();
    dataCo2.reverse();

    // RENDER GRAPHS based on selected mode
    renderGraphs(chartLabels, dataTemp, dataHum, dataMeth, dataCo2);
}

// 3. RENDER GRAPHS
// Handles destroying old charts and creating new ones based on Single vs Multi mode
function renderGraphs(labels, temp, hum, meth, co2) {
    // Destroy all existing charts to prevent "flickering" or memory leaks
    charts.forEach(c => c.destroy());
    charts = []; // Clear array

    const mode = viewModeSelect.value;

    if (mode === "single") {
        // --- SINGLE GRAPH MODE ---
        // Show single container, hide grid
        singleGraphContainer.style.display = "block";
        multiGraphGrid.style.display = "none";

        // Determine which metric to show
        const field = graphFieldSelect.value;
        
        // Pick the correct data array and color based on dropdown
        let dataToShow, colorToShow, labelToShow;
        if(field === "temperature") { dataToShow = temp; colorToShow = COLORS.temperature; labelToShow = "Temperature"; }
        else if(field === "humidity") { dataToShow = hum; colorToShow = COLORS.humidity; labelToShow = "Humidity"; }
        else if(field === "methane") { dataToShow = meth; colorToShow = COLORS.methane; labelToShow = "Methane"; }
        else if(field === "co2") { dataToShow = co2; colorToShow = COLORS.co2; labelToShow = "CO₂"; }

        // Create Chart
        const ctx = document.getElementById("mainChart").getContext("2d");
        const newChart = new Chart(ctx, {
            type: "line",
            data: {
                labels: labels,
                datasets: [{
                    label: labelToShow,
                    data: dataToShow,
                    borderColor: colorToShow,
                    backgroundColor: colorToShow.replace('1)', '0.1)'), // Same color but transparent for fill
                    borderWidth: 2,
                    fill: true,
                    tension: 0.3 // Makes line slightly curvy
                }]
            },
            options: { maintainAspectRatio: false }
        });
        charts.push(newChart);

    } else {
        // --- MULTI GRAPH MODE ---
        // Hide single container, show grid
        singleGraphContainer.style.display = "none";
        multiGraphGrid.style.display = "grid";

        // Helper to create small charts
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
                        pointRadius: 0 // Hide points on small graphs for cleanliness
                    }]
                },
                options: { 
                    maintainAspectRatio: false,
                    plugins: { legend: { display: true } } // Show legend
                }
            });
        };

        // Create the 4 charts and add them to our tracking array
        charts.push(createSmallChart("chartTemp", "Temperature", temp, COLORS.temperature));
        charts.push(createSmallChart("chartHum", "Humidity", hum, COLORS.humidity));
        charts.push(createSmallChart("chartMeth", "Methane", meth, COLORS.methane));
        charts.push(createSmallChart("chartCo2", "CO₂", co2, COLORS.co2));
    }
}