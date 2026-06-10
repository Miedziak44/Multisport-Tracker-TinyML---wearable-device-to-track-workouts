# MultiSport Tracker (Edge AI / TinyML) 

An autonomous wearable system designed for real-time fitness exercise classification and repetition counting. This project is built using **Zephyr RTOS**, machine learning via **Edge Impulse**, and the ultra-low-power **Seeed Studio XIAO nRF54L15 Sense** microcontroller.

##  Project Overview
The main goal of this project is to eliminate the need for manual workout tracking. The device (designed to be worn in an armband) utilizes an onboard accelerometer to analyze movement patterns and classify the physical activity being performed. The machine learning inference runs entirely locally on the edge device, and the results are transmitted to a smartphone application via energy-efficient Bluetooth Low Energy (BLE).

##  Key Features
* **Real-Time Classification:** Recognizes 5 distinct states: push-ups, sit-ups, jumping jacks, running, and rest (`idle`).
* **State Machine Pattern:** Implements an advanced hysteresis and digital debouncing logic (600ms cooldown) to eliminate physical noise and prevent false double-counting.
* **Sliding Window Algorithm:** Buffers raw IMU data with a 200 ms step while maintaining a 3-second window, running neural network inference 5 times per second with a minimal memory footprint.
* **Bi-directional BLE Communication (NUS):**
  * Transmits live rep counts straight to the mobile app interface.
  * Handles incoming control commands from the smartphone:
    * `1` - Resets current session counters.
    * `2` - Generates and transmits a complete workout summary log.

##  Hardware Architecture
* **Microcontroller:** Seeed Studio XIAO nRF54L15 Sense (ARM Cortex-M33 @ 128 MHz).
* **Sensor:** Integrated 6-axis IMU (accelerometer sampling frequency configured at 50 Hz).
* **Power & Enclosure:** External slim power bank layout to bypass PMIC limitations, enclosed in a rigid protective casing inside a dedicated sports armband.

##  Machine Learning Model (TinyML)
A dataset containing over 26 minutes of movement data was collected directly using the target accelerometer. Signal processing and neural network training were performed using **Edge Impulse**.
* **DSP Block:** Low-pass filter (10 Hz) + Discrete Fourier Transform (128-point FFT).
* **Neural Network Architecture (ANN):** Input layer (93 features) -> Dense (64 neurons) -> Dropout (0.2) -> Dense (32 neurons) -> Softmax (5 classes).
* **Model Accuracy (Test Set):** 97.62% Accuracy (F1-Score: 0.98).

##  Software & Compilation
* **Operating System:** Zephyr RTOS
* **Development Kit:** nRF Connect SDK v3.0.1
* **Flashing Process:** Since the nRF54L15 is a brand new SoC on the market, traditional `openocd` tools may throw IDR mismatch errors. The compiled firmware (`merged.hex`) is safely flashed using the modern `pyocd` toolchain along with target-specific hardware reset scripts:
  ```bash
  pyocd flash build/merged.hex -t nrf54l
