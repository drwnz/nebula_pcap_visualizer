import * as THREE from "three";
import { OrbitControls } from "three/addons/controls/OrbitControls.js";

let renderer;
let sensors = [];
let timeline = [];
let currentTimelineIndex = -1;

const slider = document.getElementById("slider");
const frameNumLabel = document.getElementById("frame-num");
const totalFramesLabel = document.getElementById("total-frames");
const statusLabel = document.getElementById("status");
const gridContainer = document.getElementById("viewport-grid");

init();
loadMetadata();

function init() {
  renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(window.devicePixelRatio);
  renderer.setSize(window.innerWidth, window.innerHeight);
  renderer.setScissorTest(true);
  document.getElementById("canvas-container").appendChild(renderer.domElement);

  window.addEventListener("resize", onWindowResize);
  slider.addEventListener("input", (e) =>
    loadMultiFrame(parseInt(e.target.value)),
  );
  animate();
}

async function loadMetadata() {
  try {
    const response = await fetch("data/metadata.json");
    const data = await response.json();

    timeline = data.timeline;
    totalFramesLabel.innerText = timeline.length > 0 ? timeline.length - 1 : 0;
    slider.max = timeline.length > 0 ? timeline.length - 1 : 0;

    // Initialize sensors
    sensors = data.sensors.map((s) => {
      // Create UI Container
      const container = document.createElement("div");
      container.className = "view-container";
      gridContainer.appendChild(container);

      const label = document.createElement("div");
      label.className = "view-label";
      label.innerText = `${s.name} (${s.model})`;
      container.appendChild(label);

      // Create 3D Scene
      const scene = new THREE.Scene();
      const camera = new THREE.PerspectiveCamera(75, 1, 0.1, 1000);

      const camPos = s.camera.pos;
      const camTarget = s.camera.target;
      camera.position.set(camPos[0], camPos[1], camPos[2]);

      const controls = new OrbitControls(camera, container);
      controls.target.set(camTarget[0], camTarget[1], camTarget[2]);
      controls.enableDamping = true;

      const grid = new THREE.GridHelper(100, 10, 0x444444, 0x222222);
      grid.rotation.x = Math.PI / 2;
      scene.add(grid);

      const geometry = new THREE.BufferGeometry();
      const material = new THREE.PointsMaterial({
        size: 0.1,
        vertexColors: true,
        sizeAttenuation: true,
      });
      const points = new THREE.Points(geometry, material);
      scene.add(points);

      return {
        ...s,
        container,
        scene,
        camera,
        controls,
        geometry,
        points,
        visible: true,
      };
    });

    statusLabel.innerText = `${data.project_name || "Project"} loaded. ${sensors.length} separate views.`;
    if (timeline.length > 0) loadMultiFrame(0);
  } catch (e) {
    console.error("Error loading metadata:", e);
    statusLabel.innerText = "Error loading project metadata!";
  }
}

async function loadMultiFrame(index) {
  if (index === currentTimelineIndex) return;
  currentTimelineIndex = index;
  const targetTime = timeline[index];
  frameNumLabel.innerText = index;

  const loads = sensors.map(async (sensor) => {
    // Find closest frame to targetTime
    let closest = null;
    let minDiff = Infinity;
    for (const f of sensor.frames) {
      const diff = Math.abs(Number(f.t) - Number(targetTime));
      if (diff < minDiff) {
        minDiff = diff;
        closest = f;
      }
    }

    if (!closest) return;

    try {
      const response = await fetch(`data/${closest.path}`);
      const data = await response.json();
      updatePoints(sensor, data);
    } catch (e) {
      console.warn(`Error loading frame for sensor ${sensor.name}:`, e);
    }
  });

  await Promise.all(loads);
}

function updatePoints(sensor, data) {
  const positions = new Float32Array(data.length * 3);
  const colors = new Float32Array(data.length * 3);

  data.forEach((p, i) => {
    positions[i * 3] = p.x;
    positions[i * 3 + 1] = p.y;
    positions[i * 3 + 2] = p.z;

    const val = p.i / 255.0;
    colors[i * 3] = 0.2 + val * 0.8;
    colors[i * 3 + 1] = 0.5 + val * 0.5;
    colors[i * 3 + 2] = 1.0;
  });

  sensor.geometry.setAttribute(
    "position",
    new THREE.BufferAttribute(positions, 3),
  );
  sensor.geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));
  sensor.geometry.attributes.position.needsUpdate = true;
  sensor.geometry.attributes.color.needsUpdate = true;
}

function onWindowResize() {
  renderer.setSize(window.innerWidth, window.innerHeight);
}

function animate() {
  requestAnimationFrame(animate);

  renderer.setClearColor(0x050505);
  renderer.setScissorTest(false);
  renderer.clear();
  renderer.setScissorTest(true);

  sensors.forEach((sensor) => {
    const rect = sensor.container.getBoundingClientRect();

    // Only render if visible on screen
    if (
      rect.bottom < 0 ||
      rect.top > renderer.domElement.clientHeight ||
      rect.right < 0 ||
      rect.left > renderer.domElement.clientWidth
    ) {
      return;
    }

    const width = rect.right - rect.left;
    const height = rect.bottom - rect.top;
    const left = rect.left;
    const bottom = renderer.domElement.clientHeight - rect.bottom;

    renderer.setViewport(left, bottom, width, height);
    renderer.setScissor(left, bottom, width, height);

    sensor.camera.aspect = width / height;
    sensor.camera.updateProjectionMatrix();

    sensor.controls.update();
    renderer.render(sensor.scene, sensor.camera);
  });
}
